#include <stdio.h>
#include <iostream>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <string>
#include <vector>
#include <cstring>
using namespace std;

#define MAX_LINE_LEN 15001
#define MAX_CMD_NUM 5000
#define MAX_CMD_LEN 1000
#define MAX_ARGV_LEN 256
#define MAX_PIPEFD_NUM 200
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

//正在分析的command，遇到 pipe 、numberpipe、!、> 就會跳出來，代表要先 fork 出 child process 來完成
struct ParsedCommand{

    int length = 0; //??
    char *argv[MAX_ARGV_LEN]; // 後面某些為 變數
    char filename[MAX_FILENAME_LEN]; //後面某些為 filename

}; 

void Splitcmd(char *command_line, CommandTable &command_table);
void ParseCommand(CommandTable command_table, int &operation, int &count, int &index, ParsedCommand &command_list);
void Execute(ParsedCommand command_list, int operation);


int main(int argc , char *argv[]){

    setenv("PATH", "bin:.", 1); //設定process之環境變數

    while (1){
        cout << "% ";
        char command_line[MAX_LINE_LEN]={'\0'};
        if (!cin.getline(command_line, sizeof(command_line))) {
            break;
        }

        CommandTable command_table;
        command_table.length = 0; //當 command buffer 執行完該執行的後，需要將 buffer 清空，這裡用覆蓋的方式。
        int index = 0; //當下預執行的 指令、process
        Splitcmd(command_line,command_table);

        while(index < command_table.length){
            int operation = NONE;
            int count = 0; //抓出 numberpipe 的數字
            ParsedCommand command_list;

            ParseCommand(command_table, operation, count, index, command_list);

            //cout<<"1----"<<endl;

            Execute(command_list, operation);

            //cout<<"";

        }

    }

    return 0;
}

//將輸入 command_line ，做切割，丟進 command_table 這個 buffer
void Splitcmd(char *command_line, CommandTable &command_table) {
    char delim[] = " ";
    char *pch = strtok(command_line, delim);

    while (pch != NULL) {
        command_table.commands[command_table.length++] = pch;
        pch = strtok(NULL, delim);
    }
} 

//分析 command buffer 內的關係，並將這些分析過的，丟入 command_list，
//也因為不會更改 command_table 內部，因此不使用參考。
//遇到 pipe 或其他符號，視為轉換到另一個 process，
//因此須先把當下的 process 處理完成。
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

    //??
    index++;
    command_list.argv[command_list.length] = NULL; //若是後面沒東東，上面else 還有將length＋1，因此需要null擋住
}

// 先確定 operation 是否上下正確
void Execute(ParsedCommand command_list, int operation){
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
    else{
        return;
    }
    /*
    else {
        // ChildHandler get the zombie process
        signal(SIGCHLD, ChildHandler);
        pid_t pid;

        pid = fork();
        // repeat until seccessful fork
        while (pid < 0) {
            int status;
            waitpid(-1, &status, 0);
            pid = fork();
        }
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

            if (!strcmp(command_list.argv[0], "")) {
                cerr << "Unknown command: [" << command_list.argv[0] << "]." << endl;
            } else {
                execvp(command_list.argv[0], command_list.argv);
            }

            exit(0);
        } 
        else {
            if (op == NONE || op == FILE_REDIRECTION) {
                pid_table[pid_length++] = pid;
                for (int i = 0; i < pid_length; ++i) {
                    int status;
                    waitpid(pid_table[i], &status, 0);
                }
                if (op == FILE_REDIRECTION) {
                    close(outputfd);
                }
            } else {
                pid_table[pid_length++] = pid;
            }
        }
    }
    */
}

