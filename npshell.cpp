#include <stdio.h>
#include <iostream>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <string>
#include <cstring>
using namespace std;

#define MAX_LINE_LEN 15001
#define MAX_CMD_NUM 5000
#define MAX_CMD_LEN 1000
#define MAX_ARGV_LEN 256
#define MAX_PIPE_NUM 200
#define MAX_FILENAME_LEN 1000

const int NONE = 1;
const int ORDINARY_PIPE = 2;
const int NUMBER_PIPE = 3;
const int NUMBER_PIPE_ERR = 4;
const int FILE_REDIRECTION = 5;

//未做整理的 command buffer
struct CommandTable {

    char *commands[MAX_CMD_NUM]; //array of pointers 
    int length = 0; //顯示 buffer 內裝了多少個東西

};

//正在分析的command，遇到 | 、numberpipe、!、> 就會跳出來，代表要先 fork 出 child process 來完成
struct ParsedCommand{

    int length = 0; // pipe 之前有多少指令
    char *argv[MAX_ARGV_LEN]; // 後面某些為 變數
    char filename[MAX_FILENAME_LEN]; //後面某些為 filename

}; 

// 此資料結構用於儲存系統給的 fd 編號，因為 file descriptor table 是 process 擁有，
// 而生成 pipe 結構的同時，pipe 結構之 read端 / write端 即是由 file 組成。
// 也就是說 process 擁有 fd table，當 pipe 生成時，會將其頭尾的 fd 傳給 process 的 fd table，
// 而為了達成各 process 可以透過 pipe 傳輸，需將各 process 的 fd table 做修正。
// fd[0] is read end, fd[1] is write end, 
// count is for |num and !num, 
// to_next=1 indicate '|' to_next=0 indicate "|num" or "!num".
struct Pipefd_table {
    int fd[2]; 
    int count;
    bool to_next;
};

void Splitcmd(char *command_line, CommandTable &command_table);
void ParseCommand(CommandTable command_table, int &operation, int &count, int &index, ParsedCommand &command_list);

void CreatePipefd(Pipefd_table pipefd[], int &pipe_amount, int count, bool to_next);
void ClosePipefd(Pipefd_table pipefd[], int &pipe_amount);
int GetInputfd(Pipefd_table pipefd[], int pipe_amount);
int GetOutputfd(Pipefd_table pipefd[], int &pipe_amount, int operation, int count, ParsedCommand command_list);
int GetErrorfd(Pipefd_table pipefd[], int &pipe_amount, int operation, int count);

void ChildHandler(int signo);
void Execute(ParsedCommand command_list, int operation, pid_t pid_table[], int &pid_length, int inputfd, int outputfd, int errorfd);

void CountdownPipefd(Pipefd_table pipefd[], const int pipe_amount);

int main(int argc , char *argv[]){

    int pipe_amount = 0; //紀錄存在多少 pipe 的數量
    Pipefd_table pipefd[MAX_PIPE_NUM]; //紀錄每個 pipe 的資訊
    setenv("PATH", "bin:.", 1); //設定process之環境變數

    while (1){
        cout << "% ";
        char command_line[MAX_LINE_LEN]={'\0'};
        if (!cin.getline(command_line, sizeof(command_line))) {
            break;
        }

        CommandTable command_table;
        command_table.length = 0; //當 command buffer 執行完該執行的後，需要將 buffer 清空，這裡用覆蓋的方式。
        int index = 0; // 當作觀察整行指令，後面包含哪些符號，再去判定這行該如何執行

        pid_t pid_table[MAX_CMD_NUM];
        int pid_length = 0;

        CountdownPipefd(pipefd, pipe_amount);
        Splitcmd(command_line,command_table);

        //將 command buffer 內的都執行完成
        while(index < command_table.length){
            int operation = NONE;
            int count = 0; //抓出 numberpipe 的數字
            ParsedCommand command_list;

            ParseCommand(command_table, operation, count, index, command_list);
            
            //在 execute 之前就需要設定好整個 process 的 fd
            int inputfd = GetInputfd(pipefd, pipe_amount); 
            int outputfd = GetOutputfd(pipefd, pipe_amount, operation, count, command_list);
            int errorfd = GetErrorfd(pipefd, pipe_amount, operation, count);

            Execute(command_list, operation, pid_table, pid_length, inputfd, outputfd, errorfd);            
            ClosePipefd(pipefd, pipe_amount);

        }

    }

    return 0;
}



//將輸入 command_line ，做切割，丟進 command_table 這個 buffer
void Splitcmd(char *command_line, CommandTable &command_table) {
    char delim[] = " ";
    char *temp = strtok(command_line, delim);

    while (temp != NULL) {
        command_table.commands[command_table.length++] = temp;
        temp = strtok(NULL, delim);
    }
} 

void CountdownPipefd(Pipefd_table pipefd[], const int pipe_amount) {
    for (int i = 0; i < pipe_amount; ++i) {
        pipefd[i].count -= 1;
    }
}

// 分析 command buffer 內的關係，並將這些分析過的，丟入 command_list，
// 也因為不會更改 command_table 內部，因此不使用參考。
// 遇到 pipe 或其他符號，視為轉換到另一個 process，
// 因此須先把當下的 process 處理完成。
// 也就是如 removetag test.html | number 會在 | 的地方停住
void ParseCommand(CommandTable command_table, int &operation, int &count, int &index, ParsedCommand &command_list){

    command_list.length = 0;
    command_list.argv[command_list.length++] = command_table.commands[index++]; //第一個字串必定是命令，而命令必會為 argv[0]

    while (index < command_table.length) {
        if (command_table.commands[index][0] == '|') {
            if (command_table.commands[index][1] != '\0') {
                operation = NUMBER_PIPE;
                count = atoi(&command_table.commands[index][1]);
                break;
            } else {
                operation = ORDINARY_PIPE;
                count = 0;
                break;
            }
        } //numberpipe 數字與槓會是相連的，因此字符會有非結尾
        else if (command_table.commands[index][0] == '!') {
            operation = NUMBER_PIPE_ERR;
            count = atoi(&command_table.commands[index][1]);
            break;
        } 
        else if (command_table.commands[index][0] == '>') {
            operation = FILE_REDIRECTION;
            strcpy(command_list.filename, command_table.commands[++index]); //將file資訊存到屬於filename那邊
            break;
        } 
        else {
            command_list.argv[command_list.length++] = command_table.commands[index++];
        } //??不是符號，就是其他種參數，例如:PATH、filename等等，command_list.length 就需要＋1
    }

    index++; // 以 ordinary pipe 為例，代表前兩個參數已經看過了，在包涵自己，總共就 3 個了，因此需設為下個要看的指令之index
    command_list.argv[command_list.length] = NULL; //需要null，是因為執行 execvp 指令其帶有 command 參數之結尾需有 NULL
}

void CreatePipefd(Pipefd_table pipefd[], int &pipe_amount, int count, bool to_next) {
    pipe(pipefd[pipe_amount].fd); //根據 pipe()，system 會將其 fd 幫你寫入你填入的 array
    pipefd[pipe_amount].count = count; //若是 number pipe 則用做倒數。
    pipefd[pipe_amount].to_next = to_next;
    pipe_amount++;
}

void ClosePipefd(Pipefd_table pipefd[], int &pipe_amount) {
    for (int i = 0; i < pipe_amount; ++i) 
    {
        if (pipefd[i].count <= 0 && pipefd[i].to_next == false) //number pipe
        {
            close(pipefd[i].fd[0]);
            pipefd[i].fd[0] = -1;

            pipe_amount--;

            if (pipe_amount > 0) 
            {
                Pipefd_table temp = pipefd[pipe_amount];
                pipefd[pipe_amount] = pipefd[i];
                pipefd[i] = temp;
            }

            i--;
        } 
        else if (pipefd[i].to_next == true) // ord pipe
        {
            pipefd[i].to_next = false;
        }
    }
}

// 如果是 number pipe 才需要 close 前面的
int GetInputfd(Pipefd_table pipefd[], int pipe_amount){
    for (int i = 0; i < pipe_amount; ++i) {
        // 遇到到期的 number pipe 就要去解決。
        if (pipefd[i].count == 0 && pipefd[i].to_next == false) {
            close(pipefd[i].fd[1]);
            pipefd[i].fd[1] = -1;

            return pipefd[i].fd[0];
        }
    }

    return STDIN_FILENO; //對 ordinary pipe 而言，因為與前面指令一起看，i.e. ls | 所以當然其 stdin 仍為 STDIN_FILENO
}

int GetOutputfd(Pipefd_table pipefd[], int &pipe_amount, int operation, int count, ParsedCommand command_list) {
    
    if (operation == NUMBER_PIPE || operation == NUMBER_PIPE_ERR) 
    {
        for (int i = 0; i < pipe_amount; ++i) {
            // use same count pipe which has used
            if (pipefd[i].count == count && pipefd[i].to_next == false) {
                return pipefd[i].fd[1];
            }
        }
        CreatePipefd(pipefd, pipe_amount, count, false);
        return pipefd[pipe_amount - 1].fd[1];
    } 
    else if (operation == ORDINARY_PIPE) 
    {
        CreatePipefd(pipefd, pipe_amount, count, true);
        return pipefd[pipe_amount - 1].fd[1]; //pipe 的左端(寫入pipe那端)，是pipe的尾巴。
    } 
    else if (operation == FILE_REDIRECTION) 
    {
        int fd = open(command_list.filename, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
        //對兩個檔案都需要有 r 權限 與 x 權限
        return fd;
    }
    
    return STDOUT_FILENO;
}

int GetErrorfd(Pipefd_table pipefd[], int &pipe_amount, int operation, int count) {
    if (operation == NUMBER_PIPE_ERR) {
        for (int i = 0; i < pipe_amount; ++i) {
            if (pipefd[i].count == count && pipefd[i].to_next == false) {
                return pipefd[i].fd[1];
            }
        }
        CreatePipefd(pipefd, pipe_amount, count, false);
        return pipefd[pipe_amount - 1].fd[1];
    }
    
    return STDERR_FILENO;
}

void ChildHandler(int signo) {
    // polling to wait child process, WNOHANG indicate with no hang
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0) {

    }
}

// 先確定 operation 是否上下正確
void Execute(ParsedCommand command_list, int operation, pid_t pid_table[], int &pid_length, int inputfd, int outputfd, int errorfd){
    if (!strcmp(command_list.argv[0], "exit")) {
        exit(0);
    } 
    else if (!strcmp(command_list.argv[0], "setenv")) {
        setenv(command_list.argv[1], command_list.argv[2], 1);
    } 
    else if (!strcmp(command_list.argv[0], "printenv")) {
        char *msg = getenv(command_list.argv[1]);
        if (msg) {
            cout << msg << endl;
        }
    } 
    else //unknown 與 一般指令 還須作判斷
    {
        
        bool is_unknown = true;
        char *env = getenv("PATH");
        char myenv[MAX_CMD_LEN];
        strcpy(myenv, env);
        char delim[] = ":";
        char *pch = strtok(myenv, delim);
        char command_path[MAX_CMD_LEN];

        while (pch != NULL) {
            strcpy(command_path, pch);
            //cout<<command_path<<endl;
            FILE *fp = fopen(strcat(strcat(command_path, "/"), command_list.argv[0]), "r");
            if (fp) {
                //cout<<fp<<endl;
                fclose(fp);
                is_unknown = false;
                break;
            }
            pch = strtok(NULL, delim);
        }

        if(is_unknown){
            cerr << "Unknown command: [" << command_list.argv[0] << "]." << endl;
        }
        else // 確認完，為可執行的指令，i.e. 執行檔有放在 /bin 內，再去做 fork()才比較省。
        {
            // 只有 parent 會做持續檢查，遇到 pipe ，會再次回來執行 execute 就會持續檢查有無 zombie 了。
            signal(SIGCHLD, ChildHandler);
            pid_t pid = fork();

            // 生出一個正確的 child
            while (pid < 0) {
                int status;
                waitpid(-1, &status, 0);
                pid = fork();
            }

            // pid == 0 是子程序，大於 0 則是父程序收到的子程序process id
            // 子程序若遇到 pipe 的情況，需要先如助教給的提示，
            // 子程序之
            if (pid == 0) {
                
                if (inputfd != STDIN_FILENO) {
                    dup2(inputfd, STDIN_FILENO);
                }
                if (outputfd != STDOUT_FILENO) {
                    dup2(outputfd, STDOUT_FILENO);
                }
                if (errorfd != STDERR_FILENO) {
                    dup2(errorfd, STDERR_FILENO);
                }

                if (inputfd != STDIN_FILENO) {
                    close(inputfd);
                }
                if (outputfd != STDOUT_FILENO) {
                    close(outputfd);
                }
                if (errorfd != STDERR_FILENO) {
                    close(errorfd);
                }

                execvp(command_list.argv[0], command_list.argv);
                exit(0);
            } 
            else {
                pid_table[pid_length++] = pid;
                if (operation == NONE || operation == FILE_REDIRECTION) // 指令後面沒有遇到 pipe，p.s.根據 spec，> 之後不會有 pipe，因此必定是最後程序
                {
                    for (int i = 0; i < pid_length; ++i) {
                        int status;
                        waitpid(pid_table[i], &status, 0);
                    }
                    if (operation == FILE_REDIRECTION) //已經到最終輸出了
                    {
                        close(outputfd);
                    }
                } 
                // 遇到 pipe ，父程序就先不用等“當下”這個子process處理完，因為還有後面幾個也要執行。
            }
        }
    }
}

