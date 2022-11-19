#include <stdio.h>
#include <iostream>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <string>
#include <string.h>
#include <cstring>
#include <fstream>
#include <sys/socket.h>
#include <netinet/in.h>

using namespace std;

#define MAX_LINE_LEN 15001
#define MAX_CMD_NUM 5000
#define MAX_CMD_LEN 1000
#define MAX_ARGV_LEN 256
#define MAX_PIPE_NUM 200
#define MAX_FILENAME_LEN 1000
#define ENV_NUM 100
#define ENV_LEN 500

const int NONE = 1;
const int ORDINARY_PIPE = 2;
const int NUMBER_PIPE = 3;
const int NUMBER_PIPE_ERR = 4;
const int FILE_REDIRECTION = 5;

//未做整理的 command buffer
struct CommandBuffer {

    char *commands[MAX_CMD_NUM]; //array of pointers 
    int length = 0; //顯示 buffer 內裝了多少個東西

};

//正在分析的command，遇到 | 、numberpipe、!、> 就會跳出來，代表要先 fork 出 child process 來完成
struct ParsedCommand{

    int length = 0; // pipe 之前有多少指令
    int operation = NONE; //現在這條輸入，屬於何種指令型態
    int count = 0; //抓出 numberpipe 的數字
    char *argv[MAX_ARGV_LEN]; // 指令與變數
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

struct EnvTable {
    char key[ENV_NUM][ENV_LEN];
    char value[ENV_NUM][ENV_LEN];
    int length;
};

int TCP_establish(uint16_t port);
bool IsBuildInCmd(CommandBuffer command_buffer, EnvTable &env, int sockfd);

void Splitcmd(char *input, CommandBuffer &command_buffer);
void ParseCommand(CommandBuffer command_buffer, int &index, ParsedCommand &command_list);

void CreatePipefd(Pipefd_table pipefd[], int &pipe_amount, int count, bool to_next);
void ClosePipefd(Pipefd_table pipefd[], int &pipe_amount);
void Get_StdIOfd(Pipefd_table pipefd[], int &pipe_amount, int cli_sockfd, ParsedCommand command_list, int stdIOfd[]);

void ChildHandler(int signo);
void Exe_cmd(ParsedCommand command_list, pid_t pid_table[], int &pid_length, int stdIOfd[], int cli_sockfd, EnvTable &env);

void CountdownPipefd(Pipefd_table pipefd[], const int pipe_amount);

void Exe_server(int cli_sockfd, EnvTable &env);

int main(int argc, char *argv[]){
    setenv("PATH", "bin:.", 1); //設定process之環境變數
    if (argc != 2) {
        cerr << "./np_simple [port]" << endl;
        exit(1);
    }

    int ser_sockfd;
    int cli_sockfd;
    int port = atoi(argv[1]);
    sockaddr_in cli_addr;
    socklen_t cli_addr_len;

    ser_sockfd = TCP_establish(port);

    while(true){

        cli_addr_len = sizeof(cli_addr);
        cli_sockfd = accept(ser_sockfd, (sockaddr *) &cli_addr, &cli_addr_len);
        if (cli_sockfd < 0) {
            cerr << "Error: accept failed" << endl;
            continue;
        }

        EnvTable env = {
            .length = 0
        };
        strcpy(env.key[env.length], "PATH");
        strcpy(env.value[env.length], "bin:.");
        env.length++;

        Exe_server(cli_sockfd, env);
        close(cli_sockfd);
    }
    close(ser_sockfd);
    cout<<"---------you type [Ctrl^c] to shutdown the server----------"<<endl;

    return 0;
}

int TCP_establish(uint16_t port){
    int ser_sockfd;
    int enable = 1;
    sockaddr_in ser_addr{};//memset((char *) &ser_addr, 0, sizeof(ser_addr));

    ser_sockfd = socket(AF_INET, SOCK_STREAM, 0); //選擇 ipv4, 並使用 tcp 連線，選擇 0 自動選擇對應協議
    if (ser_sockfd < 0) {
        cerr << "Error: socket failed" << endl;
        exit(1);
    }

    ser_addr.sin_family = PF_INET; //PF_INET=AF_INET
    ser_addr.sin_addr.s_addr = INADDR_ANY;
    ser_addr.sin_port = htons(port);

    if (setsockopt(ser_sockfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) < 0) //設定 socket
    {
        cerr << "Error: setsockopt failed" << endl;
        exit(1);
    }
    if (bind(ser_sockfd, (sockaddr *)&ser_addr, sizeof(ser_addr)) < 0) 
    {
        cerr << "Error: bind failed" << endl;
        exit(1);
    }
    if (listen(ser_sockfd, 0) < 0) 
    {
        cerr << "Error: listen failed" << endl;
        exit(1);
    }

    return ser_sockfd;
}

void Exe_server(int cli_sockfd, EnvTable &env){

    int pipe_amount = 0; //紀錄存在多少 pipe 的數量
    Pipefd_table pipefd[MAX_PIPE_NUM]; //紀錄每個 pipe 的資訊

    while(true){

        char read_buffer[MAX_LINE_LEN]={};
        char input[MAX_LINE_LEN]={};

        CommandBuffer command_buffer;
        command_buffer.length = 0; //當 command buffer 執行完該執行的後，需要將 buffer 清空，這裡用覆蓋的方式。
        int index = 0; // 當作觀察整行指令，後面包含哪些符號，再去判定這行該如何執行
        pid_t pid_table[MAX_CMD_NUM];
        int pid_length = 0;

        write(cli_sockfd,"% ",strlen("% "));
        do {
            memset(&read_buffer, '\0', sizeof(read_buffer));
            read(cli_sockfd, read_buffer, sizeof(read_buffer));
            strcat(input, read_buffer);

        } while (read_buffer[strlen(read_buffer) - 1] != '\n');

        strtok(input, "\r\n");

        if (!strcmp(input, "")) {
            continue;
        }
        if (!strcmp(input, "exit")) {
            return;
        }

        CountdownPipefd(pipefd, pipe_amount);
        Splitcmd(input,command_buffer);

        int last_operation = NONE;

        //將 command buffer 內的都執行完成
        while(index < command_buffer.length){

            if(!IsBuildInCmd(command_buffer, env, cli_sockfd))
            {
                ParsedCommand command_list;
                ParseCommand(command_buffer, index, command_list);

                if(last_operation == NUMBER_PIPE || last_operation == NUMBER_PIPE_ERR)
                    CountdownPipefd(pipefd, pipe_amount);//如果 number pipe 不在句尾，在count 一次
                last_operation = command_list.operation;

                int stdIOfd[3]={};
                Get_StdIOfd(pipefd, pipe_amount, cli_sockfd, command_list, stdIOfd);
                Exe_cmd(command_list, pid_table, pid_length, stdIOfd, cli_sockfd, env);            
                ClosePipefd(pipefd, pipe_amount); //close pipe 父程序才需要，因為子程序早就 exit 了
            }
            else{
                break;
            }

        }

    }

}


//將輸入 input ，做切割，丟進 command_buffer 這個 buffer
void Splitcmd(char *input, CommandBuffer &command_buffer) {
    
    char delim[] = " ";
    char *temp = strtok(input, delim);

    while (temp != NULL) {

        command_buffer.commands[command_buffer.length++] = temp;
        temp = strtok(NULL, delim);

    }

} 

void CountdownPipefd(Pipefd_table pipefd[], const int pipe_amount) {

    for (int i = 0; i < pipe_amount; ++i)
        pipefd[i].count -= 1;

}

// 分析 command buffer 內的關係，並將這些分析過的，丟入 command_list，
// 也因為不會更改 command_buffer 內部，因此不使用參考。
// 遇到 pipe 或其他符號，視為轉換到另一個 process，
// 因此須先把當下的 process 處理完成。
// 也就是如 removetag test.html | number 會在 | 的地方停住
void ParseCommand(CommandBuffer command_buffer, int &index, ParsedCommand &command_list){

    command_list.argv[command_list.length++] = command_buffer.commands[index++]; //第一個字串必定是命令，而命令必會為 argv[0]

    while (index < command_buffer.length) {

        if (command_buffer.commands[index][0] == '|') {

            if (command_buffer.commands[index][1] != '\0') {

                command_list.operation = NUMBER_PIPE;
                command_list.count = atoi(&command_buffer.commands[index][1]);
                break;

            } else {

                command_list.operation = ORDINARY_PIPE;
                command_list.count = 0;
                break;

            }
        } //numberpipe 數字與槓會是相連的，因此字符會有非結尾
        else if (command_buffer.commands[index][0] == '!') {

            command_list.operation = NUMBER_PIPE_ERR;
            command_list.count = atoi(&command_buffer.commands[index][1]);
            break;

        } 
        else if (command_buffer.commands[index][0] == '>') {

            command_list.operation = FILE_REDIRECTION;
            strcpy(command_list.filename, command_buffer.commands[++index]); //將file資訊存到屬於filename那邊
            break;

        } 
        else {

            command_list.argv[command_list.length++] = command_buffer.commands[index++];

        } //??不是符號，就是其他種參數，例如:PATH、filename等等，command_list.length 就需要＋1
    }

    index++; // 以 ordinary pipe 為例，代表前兩個參數已經看過了，在包涵自己，總共就 3 個了，因此需設為下個要看的指令之index
    command_list.argv[command_list.length] = NULL; //需要null，是因為執行 execvp 指令其帶有 command 參數之結尾需有 NULL

}

void CreatePipefd(Pipefd_table pipefd[], int &pipe_amount, ParsedCommand command_list, bool to_next) {

    pipe(pipefd[pipe_amount].fd); //根據 pipe()，system 會將其 fd 幫你寫入你填入的 array
    pipefd[pipe_amount].count = command_list.count; //若是 number pipe 則用做倒數。
    pipefd[pipe_amount].to_next = to_next;
    pipe_amount++;

}

void ClosePipefd(Pipefd_table pipefd[], int &pipe_amount) {

    for (int i = 0; i < pipe_amount; ++i) 
    {

        if (pipefd[i].count <= 0 && pipefd[i].to_next == false) //number pipe or ord pipe 
        {

            close(pipefd[i].fd[0]);

            pipe_amount-=1;

            Pipefd_table temp = pipefd[pipe_amount];
            pipefd[pipe_amount] = pipefd[i];
            pipefd[i] = temp;

            i-=1;

        } 
        else if (pipefd[i].to_next == true) // ord pipe
        {

            pipefd[i].to_next = false;

        }
    }
}

void Get_StdIOfd(Pipefd_table pipefd[], int &pipe_amount, int cli_sockfd, ParsedCommand command_list, int stdIOfd[]){
    
    stdIOfd[0] =  STDIN_FILENO; //對 ordinary pipe 而言，因為與前面指令一起看，i.e. ls | 所以當然其 stdin 仍為 STDIN_FILENO
    for (int i = 0; i < pipe_amount; ++i) {

        // 遇到到期的 pipe 就要去解決，包括 ordinary pipe;。
        if (pipefd[i].count == 0) {

            close(pipefd[i].fd[1]);
            pipefd[i].fd[1] = -1;

            stdIOfd[0] = pipefd[i].fd[0];
            break;
        }
    }

    stdIOfd[1] = cli_sockfd;
    bool find = false;
    if (command_list.operation == NUMBER_PIPE || command_list.operation == NUMBER_PIPE_ERR) 
    {

        for (int i = 0; i < pipe_amount; ++i) {

            if (pipefd[i].count == command_list.count) {

                stdIOfd[1] = pipefd[i].fd[1];
                find = true;
                break;
            }
        }

        if(!find){
            CreatePipefd(pipefd, pipe_amount, command_list, false);
            stdIOfd[1] =  pipefd[pipe_amount - 1].fd[1];
        }
    } 
    else if (command_list.operation == ORDINARY_PIPE) 
    {

        CreatePipefd(pipefd, pipe_amount, command_list, true);
        stdIOfd[1] =  pipefd[pipe_amount - 1].fd[1]; //pipe 的左端(寫入pipe那端)，是pipe的尾巴。

    } 
    else if (command_list.operation == FILE_REDIRECTION) 
    {

        stdIOfd[1] = open(command_list.filename, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
        //檔案需要有 r 權限 與 x 權限

    }

    stdIOfd[2] = cli_sockfd;
    find = false;
    if (command_list.operation == NUMBER_PIPE_ERR) {

        for (int i = 0; i < pipe_amount; ++i) {

            if (pipefd[i].count == command_list.count) {

                stdIOfd[2] =  pipefd[i].fd[1];
                find = true;
                break;
            }
        }

        if(!find){
            CreatePipefd(pipefd, pipe_amount, command_list, false);
            stdIOfd[2] =  pipefd[pipe_amount - 1].fd[1];
        }
    }

}


void ChildHandler(int signo) {
    
    // polling to wait child process, WNOHANG indicate with no hang
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0) {

    }

}

bool IsBuildInCmd(CommandBuffer command_buffer, EnvTable &env, int sockfd) {

    if (!strcmp(command_buffer.commands[0], "setenv")) {
        for (int i = 0; i < env.length; ++i) {
            if (!strcmp(env.key[i], command_buffer.commands[1])) {
                strcpy(env.value[i], command_buffer.commands[2]);
                setenv(env.key[i], env.value[i], 1);
                return true;
            }
        }
        strcpy(env.key[env.length], command_buffer.commands[1]);
        strcpy(env.value[env.length], command_buffer.commands[2]);
        env.length++;
        return true;

    } else if (!strcmp(command_buffer.commands[0], "printenv")) {
        for (int i = 0; i < env.length; ++i) {
            if (!strcmp(env.key[i], command_buffer.commands[1])) {
                write(sockfd, env.value[i], strlen(env.value[i]));
                write(sockfd, "\n", strlen("\n"));
                return true;
            }
        }
        char *myenv = getenv(command_buffer.commands[1]);
        if (myenv) {
            write(sockfd, myenv, strlen(myenv));
        }
        write(sockfd, "\n", strlen("\n"));
        return true;
    }

    return false;
}

// 先確定 operation 是否上下正確
void Exe_cmd(ParsedCommand command_list, pid_t pid_table[], int &pid_length, int stdIOfd[], int cli_sockfd, EnvTable &env){
    
    /*
    if (!strcmp(command_list.argv[0], "setenv")) {

        setenv(command_list.argv[1], command_list.argv[2], 1);

    } 
    else if (!strcmp(command_list.argv[0], "printenv")) {

        char *msg = getenv(command_list.argv[1]);
        char output[3000];
        sprintf(output, "%s\n", msg);
        if (msg) {

           write(stdIOfd[1], output, strlen(output));

        }

    } 
    
    else //unknown 與 一般指令 還須作判斷
    {
    */ 
        bool is_unknown = true;
        char myenv[MAX_CMD_LEN] = {};
        strcpy(myenv, env.value[0]);
        char delim[] = ":";
        char *pch = strtok(myenv, delim);
        char command_path[MAX_CMD_LEN];

        while (pch != NULL) {

            strcpy(command_path, pch);
            strcat(command_path, "/");
            strcat(command_path, command_list.argv[0]);
            string temp = command_path;
            fstream file;
            file.open(temp,ios::in);
            if(file){

                file.close();
                is_unknown = false;
                break;

            }
            pch = strtok(NULL, delim);

        }

        if(is_unknown){
            
            char temp[MAX_CMD_LEN]={};
            strcpy(temp,"Unknown command: [");
            strcat(temp, command_list.argv[0]);
            strcat(temp, "].\n");

            write(cli_sockfd, temp, strlen(temp));

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
                
                int x = dup2(stdIOfd[0], STDIN_FILENO); //會執行任務且會回傳東西，
                int y = dup2(stdIOfd[1], STDOUT_FILENO);
                int z = dup2(stdIOfd[2], STDERR_FILENO);

                if (stdIOfd[0] != STDIN_FILENO)
                    close(stdIOfd[0]);
                if (stdIOfd[1] != STDOUT_FILENO)
                    close(stdIOfd[1]);
                if (stdIOfd[2] != STDERR_FILENO)
                    close(stdIOfd[2]);

                while (execvp(command_path, command_list.argv) == -1) {
                    write(cli_sockfd, "error exec\n",strlen("error exec\n"));
                };
                exit(0);
            } 
            else {

                pid_table[pid_length++] = pid;
                if (command_list.operation == NONE || command_list.operation == FILE_REDIRECTION) // 指令後面沒有遇到 pipe，p.s.根據 spec，> 之後不會有 pipe，因此必定是最後程序
                {

                    for (int i = 0; i < pid_length; ++i) {

                        int status;
                        waitpid(pid_table[i], &status, 0);

                    }
                    if (command_list.operation == FILE_REDIRECTION) //已經到最終輸出了
                    {

                        close(stdIOfd[1]);

                    }
                } 
                // 遇到 pipe ，父程序就先不用等“當下”這個子process處理完，因為還有後面幾個也要執行。
            }
        }
    //}
}

