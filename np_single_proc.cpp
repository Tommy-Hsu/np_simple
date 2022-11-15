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
#include <cstdio>
#include <arpa/inet.h>

using namespace std;

#define MAX_LINE_LEN 15001
#define MAX_CMD_NUM 5000
#define MAX_CMD_LEN 1000
#define MAX_ARGV_LEN 256
#define MAX_PIPE_NUM 1024
#define MAX_FILENAME_LEN 1000
#define MAX_MSG_LEN 1024
#define MAX_NAME_LEN 20
#define MAX_USER 30
#define ENV_NUM 100
#define ENV_LEN 500

const int NONE = 1;
const int ORDINARY_PIPE = 2;
const int NUMBER_PIPE = 3;
const int NUMBER_PIPE_ERR = 4;
const int FILE_REDIRECTION = 5;
const int USER_PIPE_OUT = 6;
const int USER_PIPE_NULL = 7;

//未做整理的 command buffer
struct CommandBuffer {

    char *commands[MAX_CMD_NUM] = {}; //array of pointers 
    int length = 0; //顯示 buffer 內裝了多少個東西

};

//正在分析的command，遇到 | 、numberpipe、!、> 就會跳出來，代表要先 fork 出 child process 來完成
struct ParsedCommand{

    int argc = 0; // pipe 之前有多少指令
    int operation = NONE; //現在這條輸入，屬於何種指令型態
    int count = 0; //抓出 numberpipe 的數字
    char *argv[MAX_ARGV_LEN] = {}; // 指令與變數
    char filename[MAX_FILENAME_LEN] = {}; //後面某些為 filename

    bool rec_from_pipe = false; // 傳輸的資料是否在 user_pipe 內 ？？
    int from_user = 0;
    int to_user = 0;

}; 

// 此資料結構用於儲存系統給的 fd 編號，因為 file descriptor table 是 process 擁有，
// 而生成 pipe 結構的同時，pipe 結構之 read端 / write端 即是由 file 組成。
// 也就是說 process 擁有 fd table，當 pipe 生成時，會將其頭尾的 fd 傳給 process 的 fd table，
// 而為了達成各 process 可以透過 pipe 傳輸，需將各 process 的 fd table 做修正。
// fd[0] is read end, fd[1] is write end, 
// count is for |num and !num, 
// to_next=1 indicate '|' to_next=0 indicate "|num" or "!num".
// sockfd 確定 pipe 作用在同個 sockfd 上面，因此初值為誰不重要
struct Pipefd_table {

    int fd[2] = {}; 
    int count = 0;
    bool to_next = false;
    int sockfd = 0;
    //int last_operation = NONE; // for number pipe

};

struct Env_table {

    char key[ENV_NUM][ENV_LEN] = {};
    char value[ENV_NUM][ENV_LEN] = {};
    int amount = 0; //environments 數量

};

struct User_info {

    char name[MAX_NAME_LEN] = {};
    char ip_port[40] = {};
    int sockfd = 0;
    bool is_login = false;
    Env_table env;

};

struct UserPipe_table {

    int fd[2] = {};
    int from_user = 0;
    int to_user = 0;
};

int TCP_establish(uint16_t port);

void Welc_msg(int cli_sockfd);
void Login_broadcast(User_info user);
void Broadcast_msg(const char *msg);
int Login_user(fd_set &activefds, int ser_sockfd);
int GetUserIndex(int sockfd);
int GetUserPipeIndex(int from_user, int to_user);
void Logout_broadcast(User_info user);
void Logout_user(fd_set &activefds, int sockfd);
bool IsBuildInCmd(int sockfd, CommandBuffer command_buffer);


void Splitcmd(char *input, CommandBuffer &command_buffer);
void ParseCommand(CommandBuffer command_buffer, int &index, ParsedCommand &command_list);

void CreatePipefd(int sockfd, ParsedCommand command_list, bool to_next);
void ClosePipefd(int sockfd, int stdIOfd[], ParsedCommand command_list);
void CreateUserPipe(int from_user, int to_user);
void ClearPipefd(int sockfd);
void Get_StdIOfd(int sockfd, int stdIOfd[], ParsedCommand command_list);

void ChildHandler(int signo);
void Exe_cmd(ParsedCommand command_list, pid_t pid_table[], int &pid_length, int stdIOfd[], int sockfd);
void CountdownPipefd(int sockfd);

User_info users[MAX_USER + 1]; //好多個 user_info
Pipefd_table pipefd[MAX_PIPE_NUM];
UserPipe_table user_pipes[MAX_PIPE_NUM];
int pipe_amount = 0;
int user_pipe_amount = 0;
char raw_command[MAX_CMD_NUM] = {};  //?

int main(int argc, char *argv[]){
    setenv("PATH", "bin:.", 1); //設定process之環境變數
    if (argc != 2) {
        cerr << "./np_single_proc [port]" << endl;
        exit(1);
    }

    int ser_sockfd;
    int port = atoi(argv[1]);
    //因為在 select （遍歷接受訊號） 時候，fds 會被修改狀態，因此需要一個被操作(readfds)，一個用來永久存(activefds)
    fd_set readfds;
    fd_set activefds;
    int nfds = getdtablesize(); //獲得本身的 fd table size; //fds 的數量

    ser_sockfd = TCP_establish(port);

    FD_ZERO(&activefds); //初始
    FD_SET(ser_sockfd, &activefds); //監聽 ser_sockfd

    while(true){

        memcpy(&readfds, &activefds, sizeof(readfds)); //copy activefds to readfds，每輪全部從心檢查

        if(select(nfds, &readfds, NULL, NULL, NULL) < 0)
        {
            cerr << "Error: select error" << endl;
            continue;
        }

        if(FD_ISSET(ser_sockfd, &readfds)) 
        {
            
            int cli_sockfd = Login_user(activefds, ser_sockfd); //新登入使用者當然是要被永久服務
            write(cli_sockfd, "% ", strlen("% "));

        }
        //有 telnet 過來時，ser_sockfd 已經在 activefds 內，
        //每次while時，readfds 被更新，因此一定會發現有 telnet 來
        //select 是每一輪都會檢查所有接口
        //而 fd_isset 就是特別去確認 ser_sockfd 這欄位是否更新狀態，
        //因為代表有新 user 要進來系統

        //接下來去檢查其他有訊號的 socket
        //包括 cli_sockfd
        for(int sockfd = 0; sockfd < nfds; ++sockfd){
            if(sockfd != ser_sockfd && FD_ISSET(sockfd, &readfds))
            {
                char read_buffer[MAX_LINE_LEN]={}; //raw input
                char input[MAX_LINE_LEN]={}; // raw input to clear input
                CommandBuffer command_buffer;
                command_buffer.length = 0; //當 command buffer 執行完該執行的後，需要將 buffer 清空，這裡用覆蓋的方式。
                
                do {
                    memset(&read_buffer, '\0', sizeof(read_buffer));
                    read(sockfd, read_buffer, sizeof(read_buffer));
                    strcat(input, read_buffer);

                } while (read_buffer[strlen(read_buffer) - 1] != '\n');

                strtok(input, "\r\n");
                strcpy(raw_command, input); //回報錯誤訊息
                Splitcmd(input,command_buffer);

                if (command_buffer.length == 0)
                    continue;
                
                if (!strcmp(command_buffer.commands[0], "exit")) {
                    ClearPipefd(sockfd);
                    Logout_user(activefds, sockfd);
                    continue;
                }

                CountdownPipefd(sockfd);

                if (!IsBuildInCmd(sockfd, command_buffer)) {

                    int index = 0; // 當作觀察整行指令，後面包含哪些符號，再去判定這行該如何執行
                    pid_t pid_table[MAX_CMD_NUM]={};
                    int pid_length = 0;
                    while (index < command_buffer.length)
                    {
                        int stdIOfd[3]={};
                        ParsedCommand command_list;
                        ParseCommand(command_buffer, index, command_list);

                        Get_StdIOfd(sockfd, stdIOfd, command_list);

                        Exe_cmd(command_list, pid_table, pid_length, stdIOfd, sockfd);

                        ClosePipefd(sockfd, stdIOfd, command_list);

                        cout<<"";

                    }
                }
                write(sockfd, "% ", strlen("% "));
            }
        }
    }
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

    if (setsockopt(ser_sockfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0) //設定 socket
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

void Welc_msg(int cli_sockfd){
    char msg[] = "****************************************\n** Welcome to the information server. **\n****************************************\n";
    write(cli_sockfd, msg, strlen(msg));
}

void Broadcast_msg(const char *msg){
    for (int i = 1; i <= MAX_USER; ++i) {
        if (users[i].is_login) {
            write(users[i].sockfd, msg, strlen(msg));
        }
    }
}

void Login_broadcast(User_info user){
    char msg[2000];
    sprintf(msg, "*** User '%s' entered from %s. ***\n", user.name, user.ip_port);
    Broadcast_msg(msg);
}

void Logout_broadcast(User_info user) {
    char msg[2000];
    sprintf(msg, "*** User '%s' left. ***\n", user.name);
    Broadcast_msg(msg);
}

int Login_user(fd_set &activefds, int ser_sockfd) {

    int cli_sockfd;
    sockaddr_in cli_addr;
    socklen_t cli_addr_len = sizeof(cli_addr);

    cli_sockfd = accept(ser_sockfd, (sockaddr *) &cli_addr, &cli_addr_len);
    if (cli_sockfd < 0) {
        cerr << "Error: accept error" << endl;
        exit(1);
    }

    FD_SET(cli_sockfd, &activefds);

    Welc_msg(cli_sockfd);

    // user init
    for (int i = 1; i <= MAX_USER; ++i) {
        if (users[i].is_login == false)  //從 users 中找最近空的 user_info 欄位給薪來的 
        {
            strcpy(users[i].name, "(no name)");
            char port[10];
            sprintf(port, "%d", ntohs(cli_addr.sin_port));
            strcpy(users[i].ip_port, inet_ntoa(cli_addr.sin_addr));
            strcat(users[i].ip_port, ":");
            strcat(users[i].ip_port, port);
            users[i].sockfd = cli_sockfd;
            users[i].is_login = true;
            strcpy(users[i].env.key[users[i].env.amount], "PATH");
            strcpy(users[i].env.value[users[i].env.amount], "bin:.");
            users[i].env.amount++;

            Login_broadcast(users[i]);
            break;
        }
    }

    return cli_sockfd;

}

void Logout_user(fd_set &activefds, int sockfd){
    FD_CLR(sockfd, &activefds);
    int idx = GetUserIndex(sockfd);
    if (idx != -1) {
        users[idx].is_login = false;
        close(users[idx].sockfd);
        users[idx].sockfd = -1;
        Logout_broadcast(users[idx]);
    }
}

int GetUserIndex(int sockfd) {
    for (int i = 1; i <= MAX_USER; ++i) {
        if (sockfd == users[i].sockfd && users[i].is_login) {
            return i;
        }
    }
    return -1;
}

int GetUserPipeIndex(int from_user, int to_user) {

    for (int i = 0; i < user_pipe_amount; ++i) {

        if (user_pipes[i].from_user == from_user && user_pipes[i].to_user == to_user) {
            return i;
        }
    }
    return -1;
}

bool IsBuildInCmd(int sockfd, CommandBuffer command_buffer) {  

    if (!strcmp(command_buffer.commands[0], "yell")) {

        char output[MAX_MSG_LEN]={};
        int idx = GetUserIndex(sockfd);
        if (idx != -1) {
            sprintf(output, "*** %s yelled ***: %s\n", users[idx].name, command_buffer.commands[1]);
            Broadcast_msg(output);
        }

    } else if (!strcmp(command_buffer.commands[0], "name")) {

        char output[MAX_MSG_LEN]={};
        int idx = GetUserIndex(sockfd);
        bool name_is_exit = false;
        if (idx != -1) {
            for (int i = 1; i <= MAX_USER; ++i) {
                if (!strcmp(users[i].name, command_buffer.commands[1])) {
                    sprintf(output, "*** User '%s' already exists. ***\n", users[i].name);
                    write(sockfd, output, strlen(output));
                    name_is_exit = true;
                    break;
                }
            } 
            if(!name_is_exit){
                strcpy(users[idx].name, command_buffer.commands[1]);
                sprintf(output, "*** User from %s is named '%s'. ***\n", users[idx].ip_port, users[idx].name);
                Broadcast_msg(output);
            }
        }

    } else if (!strcmp(command_buffer.commands[0], "who")) {

        char output[MAX_MSG_LEN]={};
        strcpy(output, "<ID>\t<nickname>\t<IP:port>\t<indicate me>\n");
        write(sockfd, output, strlen(output));

        for (int i = 1; i <= MAX_USER; ++i) {
            if (users[i].is_login) {
                if (users[i].sockfd == sockfd) {
                    sprintf(output, "%d\t%s\t%s\t%s\n", i, users[i].name, users[i].ip_port, "<-me");
                } else {
                    sprintf(output, "%d\t%s\t%s\n", i, users[i].name, users[i].ip_port);
                }
                write(sockfd, output, strlen(output));
            }
        }

    } else if (!strcmp(command_buffer.commands[0], "tell")) {

        char output[MAX_MSG_LEN]={};
        int idx = GetUserIndex(sockfd);
        if (users[atoi(command_buffer.commands[1])].is_login) {
            sprintf(output, "*** %s told you ***: %s\n", users[idx].name, command_buffer.commands[2]);
            write(users[atoi(command_buffer.commands[1])].sockfd, output, strlen(output));
        } else {
            sprintf(output, "*** Error: user #%d does not exist yet. ***\n", atoi(command_buffer.commands[1]));
            write(sockfd, output, strlen(output));
        }

    } else if (!strcmp(command_buffer.commands[0], "printenv")) {

        int idx = GetUserIndex(sockfd);
        int have_not_pri = true;
        for (int i = 0; i < users[idx].env.amount; ++i) {
            if (!strcmp(command_buffer.commands[1], users[idx].env.key[i])) {
                write(sockfd, users[idx].env.value[i], strlen(users[idx].env.value[i]));
                write(sockfd, "\n", strlen("\n"));
                have_not_pri = false;
                break;
            }
        }
        if(have_not_pri){
            char *env = getenv(command_buffer.commands[1]);
            write(sockfd, env, strlen(env));
            write(sockfd, "\n", strlen("\n"));
        }

        cout<<"";

    } else if (!strcmp(command_buffer.commands[0], "setenv")) {

        int idx = GetUserIndex(sockfd);
        int have_not_writen = true;
        for (int i = 0; i < users[idx].env.amount; ++i) {
            if (!strcmp(command_buffer.commands[1], users[idx].env.key[i])) {
                strcpy(users[idx].env.value[i], command_buffer.commands[2]);
                have_not_writen = false;
                break;
            }
        }

        if(have_not_writen){
            strcpy(users[idx].env.key[users[idx].env.amount], command_buffer.commands[1]);
            strcpy(users[idx].env.value[users[idx].env.amount], command_buffer.commands[2]);
            users[idx].env.amount++;
        }

    } else {
        return false;
    }

    return true;
}

//將輸入 input ，做切割，丟進 command_buffer 這個 buffer
void Splitcmd(char *input, CommandBuffer &command_buffer) {
    
    char delim[] = " \n";
    char *temp = strtok(input, delim);
    bool isBuildIn = false;

    while (temp != NULL) {
        command_buffer.commands[command_buffer.length++] = temp;
        if ((!strcmp(command_buffer.commands[0], "yell") && command_buffer.length == 1) || 
            (!strcmp(command_buffer.commands[0], "name") && command_buffer.length == 1) ||
            (!strcmp(command_buffer.commands[0], "tell") && command_buffer.length == 2) ||
            (!strcmp(command_buffer.commands[0], "printenv") && command_buffer.length == 1) ||
            (!strcmp(command_buffer.commands[0], "setenv") && command_buffer.length == 2)) 
            {
                isBuildIn = true;
                break;
            }
        temp = strtok(NULL, delim);

    }
    if (isBuildIn) {
        temp = strtok(NULL, "\n");
        command_buffer.commands[command_buffer.length++] = temp;
    }

} 

void CountdownPipefd(int sockfd) {

    // pipe_amount 為全域變數，就不用再次寫入參數了
    for (int i = 0; i < pipe_amount; ++i)
        if(pipefd[i].sockfd == sockfd)
            pipefd[i].count -= 1;

}

// 分析 command buffer 內的關係，並將這些分析過的，丟入 command_list，
// 也因為不會更改 command_buffer 內部，因此不使用參考。
// 遇到 pipe 或其他符號，視為轉換到另一個 process，
// 因此須先把當下的 process 處理完成。
// 也就是如 removetag test.html | number 會在 | 的地方停住
void ParseCommand(CommandBuffer command_buffer, int &index, ParsedCommand &command_list){

    command_list.argv[command_list.argc++] = command_buffer.commands[index++]; //第一個字串必定是命令，而命令必會為 argv[0]

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

            if(command_buffer.commands[index][1] == '\0'){

                command_list.operation = FILE_REDIRECTION;
                strcpy(command_list.filename, command_buffer.commands[++index]); //將file資訊存到屬於filename那邊
                break;
            }
            
            command_list.operation = USER_PIPE_OUT;
            command_list.to_user = atoi(&command_buffer.commands[index][1]);

            //??
            if(command_buffer.length > index + 1 && command_buffer.commands[index + 1][0] == '<'){
                index++;
                continue;
            }

            break;

        } 
        else if (command_buffer.commands[index][0] == '<') {

            //command_list.operation ??
            command_list.from_user = atoi(&command_buffer.commands[index][1]);
            command_list.rec_from_pipe = true;
            index++;
        }
        else {

            command_list.argv[command_list.argc++] = command_buffer.commands[index++];

        } //??不是符號，就是其他種參數，例如:PATH、filename等等，command_list.length 就需要＋1
        
    }

    index++; // 以 ordinary pipe 為例，代表前兩個參數已經看過了，在包涵自己，總共就 3 個了，因此需設為下個要看的指令之index
    command_list.argv[command_list.argc] = NULL; //需要null，是因為執行 execvp 指令其帶有 command 參數之結尾需有 NULL

}

void CreatePipefd(int sockfd, ParsedCommand command_list, bool to_next) {

    pipe(pipefd[pipe_amount].fd); //根據 pipe()，system 會將其 fd 幫你寫入你填入的 array
    pipefd[pipe_amount].count = command_list.count; //若是 number pipe 則用做倒數。
    pipefd[pipe_amount].to_next = to_next;
    pipefd[pipe_amount].sockfd = sockfd;
    pipe_amount++;

}

void CreateUserPipe(int from_user, int to_user) {

    user_pipes[user_pipe_amount].from_user = from_user;
    user_pipes[user_pipe_amount].to_user = to_user;
    pipe(user_pipes[user_pipe_amount].fd);
    user_pipe_amount++;

}

void ClearPipefd(int sockfd) {

    for (int i = 0; i < pipe_amount; ++i) {

        if (pipefd[i].sockfd == sockfd) {

            close(pipefd[i].fd[0]);
            close(pipefd[i].fd[1]);

            pipe_amount -= 1;

            Pipefd_table temp = pipefd[pipe_amount];
            pipefd[pipe_amount] = pipefd[i];
            pipefd[i] = temp;
            i -= 1;

        }
    }

    int idx = GetUserIndex(sockfd);
    for (int i = 0; i < user_pipe_amount; ++i) {

        if (user_pipes[i].from_user == idx || user_pipes[i].to_user == idx) {

            close(user_pipes[i].fd[0]);
            close(user_pipes[i].fd[1]);

            user_pipe_amount -= 1;
            UserPipe_table temp = user_pipes[user_pipe_amount];
            user_pipes[user_pipe_amount] = user_pipes[i];
            user_pipes[i] = temp;
            i -= 1;

        }
    }
}

void ClosePipefd(int sockfd, int stdIOfd[], ParsedCommand command_list) {

    for (int i = 0; i < pipe_amount; ++i) {

        if (pipefd[i].sockfd == sockfd) {

            if (pipefd[i].count <= 0 && pipefd[i].to_next == false) {

                close(pipefd[i].fd[0]);
                
                pipe_amount -= 1;

                Pipefd_table temp = pipefd[pipe_amount];
                pipefd[pipe_amount] = pipefd[i];
                pipefd[i] = temp;

                i -= 1;

            } else if (pipefd[i].to_next == true) {
                pipefd[i].to_next = false;
            }
        }
    }

    if (command_list.rec_from_pipe) {

        int to_user = GetUserIndex(sockfd), from_user = command_list.from_user;
        int user_pipe_idx = GetUserPipeIndex(from_user, to_user);
        if (user_pipe_idx != -1) {

            close(user_pipes[user_pipe_idx].fd[0]);
            UserPipe_table temp = user_pipes[user_pipe_amount - 1];
            user_pipes[user_pipe_amount - 1] = user_pipes[user_pipe_idx];
            user_pipes[user_pipe_idx] = temp;
            user_pipe_amount--;
        } else {
            close(stdIOfd[0]);
        }
    }
}

void Get_StdIOfd(int sockfd, int stdIOfd[], ParsedCommand command_list){
    
    // pipe_amount 與 pipefd[] 都已經變成全域變數，不用在加入參數了
    // sockfd, command_list, stdiofd[] 都是隨著迴圈變動，因此須為區域變數
    stdIOfd[0] =  STDIN_FILENO;
    if(command_list.rec_from_pipe){
        
        char msg[3000];
        int from_user = command_list.from_user, to_user = GetUserIndex(sockfd);

        if (users[from_user].is_login == false) {
            
            sprintf(msg, "*** Error: user #%d does not exist yet. ***\n", from_user);
            write(sockfd, msg, strlen(msg));
            stdIOfd[0] = open("/dev/null", O_RDONLY);

        } 
        else {
            
            int user_pipe_idx = GetUserPipeIndex(from_user, to_user);
            
            if (user_pipe_idx == -1) {
                sprintf(msg, "*** Error: the pipe #%d->#%d does not exist yet. ***\n", from_user, to_user);
                write(sockfd, msg, strlen(msg));
                stdIOfd[0] = open("/dev/null", O_RDONLY);
                
            } else {

                sprintf(msg, "*** %s (#%d) just received from %s (#%d) by '%s' ***\n", users[to_user].name, to_user, users[from_user].name, from_user, raw_command);
                Broadcast_msg(msg);
                close(user_pipes[user_pipe_idx].fd[1]);
                stdIOfd[0] = user_pipes[user_pipe_idx].fd[0];
            }
        }
    }
    else{

        for (int i = 0; i < pipe_amount; ++i) {

            // 遇到到期的 pipe 就要去解決，包括 ordinary pipe;。
            if (pipefd[i].count == 0 && pipefd[i].sockfd == sockfd) {

                close(pipefd[i].fd[1]);
                pipefd[i].fd[1] = -1;

                stdIOfd[0] = pipefd[i].fd[0];
                break;
            }
        }
    }

    stdIOfd[1] = sockfd;
    bool find = false;
    if (command_list.operation == NUMBER_PIPE || command_list.operation == NUMBER_PIPE_ERR) 
    {

        for (int i = 0; i < pipe_amount; ++i) {

            if (pipefd[i].count == command_list.count && pipefd[i].sockfd == sockfd) {

                stdIOfd[1] = pipefd[i].fd[1];
                find = true;
                break;
            }
        }

        if(!find){
            CreatePipefd(sockfd, command_list, false);
            stdIOfd[1] =  pipefd[pipe_amount - 1].fd[1];
        }
    } 
    else if (command_list.operation == ORDINARY_PIPE) 
    {

        CreatePipefd(sockfd, command_list, true);
        stdIOfd[1] =  pipefd[pipe_amount - 1].fd[1]; //pipe 的左端(寫入pipe那端)，是pipe的尾巴。

    } 
    else if (command_list.operation == FILE_REDIRECTION) 
    {

        stdIOfd[1] = open(command_list.filename, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
        //檔案需要有 r 權限 與 x 權限

    }
    else if (command_list.operation == USER_PIPE_OUT){

        char msg[3000] = {};
        int from_user = GetUserIndex(sockfd), to_user = command_list.to_user;
        if (to_user > MAX_USER || users[to_user].is_login == false) {

            sprintf(msg, "*** Error: user #%d does not exist yet. ***\n", to_user);
            write(sockfd, msg, strlen(msg));
            command_list.operation = USER_PIPE_NULL;
            stdIOfd[1] = open("/dev/null", O_WRONLY);
            
        } else {

            int user_pipe_idx = GetUserPipeIndex(from_user, to_user);
            if (user_pipe_idx != -1) {

                sprintf(msg, "*** Error: the pipe #%d->#%d already exists. ***\n", from_user, to_user);
                write(sockfd, msg, strlen(msg));
                command_list.operation = USER_PIPE_NULL;
                stdIOfd[1] = open("/dev/null", O_WRONLY);

            } 
            else {

                sprintf(msg, "*** %s (#%d) just piped '%s' to %s (#%d) ***\n", users[from_user].name, from_user, raw_command, users[to_user].name, to_user);
                Broadcast_msg(msg);
                CreateUserPipe(from_user, to_user);
                stdIOfd[1] = user_pipes[user_pipe_amount - 1].fd[1];

            }
        }

    }

    stdIOfd[2] = sockfd;
    find = false;
    if (command_list.operation == NUMBER_PIPE_ERR) {

        for (int i = 0; i < pipe_amount; ++i) {

            if (pipefd[i].count == command_list.count && pipefd[i].sockfd == sockfd) {

                stdIOfd[2] =  pipefd[i].fd[1];
                find = true;
                break;
            }
        }

        if(!find){
            CreatePipefd(sockfd, command_list, false);
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

// 先確定 operation 是否上下正確
void Exe_cmd(ParsedCommand command_list, pid_t pid_table[], int &pid_length, int stdIOfd[], int sockfd){
    
    char command_path[MAX_CMD_LEN] = {};
    bool is_unknown = true;

    int idx = GetUserIndex(sockfd);
    if (idx != -1) {

        char myenv[MAX_CMD_LEN] = {};
        strcpy(myenv, users[idx].env.value[0]);
        char delim[] = ":";
        char *pch = strtok(myenv, delim);

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
    }

    if(is_unknown){
        
        char temp[MAX_CMD_LEN]={};
        strcpy(temp,"Unknown command: [");
        strcat(temp, command_list.argv[0]);
        strcat(temp, "].\n");

        write(sockfd, temp, strlen(temp));

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
                write(sockfd, "error exec\n",strlen("error exec\n"));
            };
            exit(0);
        } 
        else {

            pid_table[pid_length++] = pid;

            if (command_list.operation == NONE) {
                //pid_table[pid_length++] = pid;
                for (int i = 0; i < pid_length; ++i) {
                    int status;
                    waitpid(pid_table[i], &status, 0);
                }
            }
            if (command_list.operation == FILE_REDIRECTION) {
                close(stdIOfd[1]);
            }
            if (command_list.operation == USER_PIPE_NULL) {
                //pid_table[pid_length++] = pid;
                for (int i = 0; i < pid_length; ++i) {
                    int status;
                    waitpid(pid_table[i], &status, 0);
                }
                close(stdIOfd[1]);
            }
            // 遇到 pipe ，父程序就先不用等“當下”這個子process處理完，因為還有後面幾個也要執行。
        }
    }

}

