#include <sstream>
#include <stdio.h>
#include <iostream>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <queue>
#include <vector>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/ipc.h>
#include <sys/shm.h>

using namespace std;

#define MAX_USER 30
#define MAX_MSG_LEN 1024
#define MAX_PATH_SIZE 30
#define SHM_KEY 1111
#define SHM_MSG_KEY 2222
#define SHM_FIFO_KEY 3333
#define FIFO_PATH "user_pipe/"

struct PipeFd {
    int in_fd = 0;
    int out_fd = 0;
    int count = 0;
};
struct Client_info {
    int id = 0;
    int pid = 0;
    int valid = 0; 
    char user_name[20] = {};
    char user_ip[INET_ADDRSTRLEN] = {};
    int port = 0;
};
struct FIFO {
    int in_fd = 0;
    int out_fd = 0;
    char name[MAX_PATH_SIZE] = {};
    bool is_used = false;
};

// fifo 二維陣列 原因
struct FIFOInfo {
    FIFO fifo[MAX_USER][MAX_USER];
};

// global server variable
// 目前看來，共享記憶體是一個已經選定的區域，並且每次連結寫入會覆蓋上次操作的，不過要斷開
int g_shmid_cli = 0;
int g_shmid_msg = 0;
int g_shmid_fifo = 0;

// client / current_cli_id 現在在操作的 cli_id
int cur_id = 0;

// create user variable
vector<Client_info> cli_table;

int TCP_establish(uint16_t port);
Client_info* GetCliSHM(int g_shmid_cli);
Client_info* GetClientByID(int id, Client_info*);
char* GetMsgSHM(int g_shmid_msg);
FIFOInfo* GetFIFOSHM(int g_shmid_fifo);
int GetIDFromSHM();
void Broadcast(string action, string msg, int cur_id, int target_id);
void BroadcastOne(string action, string msg, int cur_id, int target_id);
void PrintWelcome();
void SigHandler(int sig);
void AddClient(int id, int sockfd, sockaddr_in address);
void Server_sig_handler (int sig); //事先設定 中斷 ctrl+c 之處理
void ResetCliSHM(int g_shmid_cli);
void ResetFIFOSHM(int g_shmid_fifo);
void Init_SHM();
void DelSHM();

// 宣告
class Shell {
    private:

        // path
        vector<string> path_vector_;

        // commands
        string terminal_input_;
        queue<string> cmds_;
        
        // pid
        vector<pid_t> pid_vector_;
        
        // numbered pipe
        vector<PipeFd> pipe_vector_;
    
    public:
        void Exec(int);

        // built-in ops
        vector<string> Split(string, string);
        void SetEnv(string, string);
        void PrintEnv(string);
        
        // command ops
        void ParseArgs();
        int ExecCmds();
        bool IsQueueEmpty(queue<string>);
        int ExecCmd(vector<string> &, bool, int, int, int, bool, bool, int);
        bool IsExecutable(string, vector<string>&);
        
        // pipe ops
        void CreatePipe(vector<PipeFd>&, int, int&);
        void CountdownPipefd(vector<PipeFd>&);
        bool GetPipeFd(vector<PipeFd>&, int&);
        void BindPipeFd(int, int&);
        void ConnectPipeFd(int, int, int);
        void ErasePipefd(vector<PipeFd>&);
        
        // pid ops
        static void ChildHandler(int);
        
        // client ops
        int ClientExec(int);
        void Who(void);
        void Yell(string);
        void Tell(int, string);
        void Name(string);
        void SetAllEnv(void);
        void EraseEnv(string);
        
        // client user pipe
        void CreateUserPipe(int, int, int&);
        void GetUserPipeFd(int, int, int&);
        void SetUserPipeOut(int send_id, int& out_fd);
        void EraseUserPipe(int);

};

// slave 中真正處理使用者
// 處理程序集中在 execCMDs 中
int Shell::ClientExec(int id) {

    cur_id = id;  // current id
    clearenv(); // 此 slave 的 pid 下的環境，因為只有一個環境，當然不用指定參數
    SetEnv("PATH", "bin:.");

    // 設定 signal handler（用來接受），kill(信號) 才是發送
    signal(SIGUSR1, SigHandler); // receive messages from others，自行定義 signal，由 kill 呼叫 
	signal(SIGUSR2, SigHandler); // open fifos to read，自行定義 signal，由 kill 呼叫

	signal(SIGINT, SigHandler); // ctrl+c
	signal(SIGQUIT, SigHandler); // ctrl+ "\"
	signal(SIGTERM, SigHandler); // 軟體終止程序

    PrintWelcome();
    Broadcast("login", "", cur_id, -1);
    pid_vector_.clear();

    while (true) {
        cout << "% ";

        getline(cin, terminal_input_);
        if(cin.eof()) {
            cout << endl;
            return 1;
        }
        if(terminal_input_.empty()) {
            continue;
        }

        ParseArgs();

        if (ExecCmds() == -1) {
            return -1;
        }
    }
}

// 因為還需要獲得之前的 path 字串
// 因此使用 vector path_vector 去儲存過去的東東
void Shell::SetEnv(string var, string val) {

    setenv(var.c_str(), val.c_str(), 1);

    if (var == "PATH") {
        path_vector_.clear();
        vector<string> res = Split(val, ":");
        for(vector<string>::iterator it = res.begin(); it != res.end(); ++it) {
            path_vector_.push_back(*it);
        }
    }

}

// 幫助切割（可能有更好的切法）
vector<string> Shell::Split(string s, string delimiter) {

    size_t pos_start = 0, pos_end, delim_len = delimiter.length();
    string token;
    vector<string> res;
    while ((pos_end = s.find(delimiter, pos_start)) != string::npos) {
        token = s.substr(pos_start, pos_end - pos_start);
        pos_start = pos_end + delim_len;
        res.push_back(token);
    }
    res.push_back(s.substr (pos_start));
    return res;
}

// cout env
void Shell::PrintEnv(string var) {
    char* val = getenv(var.c_str());
    if (val != NULL) {
        cout << val << endl;
    }
}

// 連動 shm_cli 並 loop 出每個
void Shell::Who(void) {
    int temp_id = 0;
    Client_info* shm_cli;
    Client_info* cur_cli = GetClientByID(cur_id, shm_cli); //shm_cli 是誰都不重要，此函式固定呼叫 g_shmid_cli

    cout << "<ID>\t" << "<nickname>\t" << "<IP:port>\t"<<"<indicate me>"<< endl;

    for (size_t id = 0; id < MAX_USER; ++id) 
    {
        if (GetClientByID(id + 1, shm_cli) != NULL) // 把所有存在的人叫出
        {
            temp_id = id + 1;
            Client_info* temp = GetClientByID(temp_id, shm_cli);
            cout << temp->id << "\t" << temp->user_name << "\t" << temp->user_ip << ":" << temp->port;

            if (temp_id == cur_cli->id) 
            {
                cout << "\t" << "<-me" << endl;
            } 
            else 
            {
                cout << "\t" << endl;
            }
        }
    }
    shmdt(shm_cli);
}

// 呼叫 broadcast
void Shell::Yell(string msg) {
    Broadcast("yell", msg, cur_id, -1);
}

// tell 只會去 broadcastone 
void Shell::Tell(int target_id, string msg) {

    // attach to  shared memory
    Client_info* shm_cli = GetCliSHM(g_shmid_cli);
    if(shm_cli[target_id-1].valid != 0) 
    {
        BroadcastOne("tell", msg, cur_id, target_id);
    } 
    else 
    {
        cerr << "*** Error: user #" << to_string(target_id) << " does not exist yet. ***" << endl;
    }
    shmdt(shm_cli);
}

// 外面會傳入目標，先判斷有無用過，再去更改
void Shell::Name(string name) {

    Client_info* shm_cli = GetCliSHM(g_shmid_cli);
    for (size_t i = 0; i < MAX_USER; ++i) 
    {
        if (shm_cli[i].user_name == name) 
        {
            cout << "*** User '" + name + "' already exists. ***" << endl;
            return;
        }
    }
    strcpy(shm_cli[cur_id-1].user_name, name.c_str());
    shmdt(shm_cli);
    Broadcast("name", "", cur_id, -1);
}

// 因為是 class 自己內部的變數與函數，因此可以直接呼叫
// istringstream 是從 string 對象中讀取
// 此法可以視為“切割”
void Shell::ParseArgs() {

    istringstream in(terminal_input_);
    string t;
    while (in >> t) {
        cmds_.push(t);
    }
}

// 主要 shell 集中執行
int Shell::ExecCmds() {

    // store arguments
    bool is_first_argv = true;
    bool is_final_argv = false; // 去做事
    string prog;
    vector<string> arguments;

    // pid
    bool is_using_pipe = false;
    bool line_ends = false; // 真正判斷一行結束
    bool is_in_redirect = false;
    int in_fd = STDIN_FILENO;
    int out_fd = STDOUT_FILENO;
    int err_fd = STDERR_FILENO;

    // client status
    int status = 0;
    bool is_in_userpipe = false;

    // client broadcast msg and setup userpipe
    int source_id = -1; //代表有從其他地方 source 接受到 userpipe 的資訊
    int target_id = -1; //有發送到其他地方 target 資訊，使用 userpipe

    // client broadcast msg
    int recv_str_id = -1;
    int send_str_id = -1;


    // cmds_ 就是一個 shell 內的 queue 並對他操作
    // 剛開始讀取指令，啥都不知道，因此所有條件初始為 false
    // 在 parse_ags 已經將 input 分成 strings 放入 cmds_
    // 此處操作 cmds_ 會隨之變動
    while (!cmds_.empty()) {
        
        // 初始 fd，可發現前面都還沒初始
        if (!is_in_redirect && !is_in_userpipe) {
            in_fd = STDIN_FILENO;
            out_fd = STDOUT_FILENO;
            err_fd = STDERR_FILENO;
        }

        // 初始接受指令
        if (is_first_argv) 
        {
            prog = cmds_.front();
            cmds_.pop();
            arguments.clear();
            arguments.push_back(prog);
            
            if (prog == "tell" || prog == "yell") 
            {

                while (!cmds_.empty()) 
                {
                    arguments.push_back(cmds_.front());
                    cmds_.pop();
                }
            }

            is_first_argv = false;
            is_final_argv = IsQueueEmpty(cmds_); // ls
            is_using_pipe = false; // 第一個指令還無法判斷
            if (cmds_.empty()) {
                line_ends = true;
            }
        } 
        else // 處理不是第一個指令參數，像是 target_id,is_using_pipe 等等，主要修改 in_fd, out_fd 的問題
        {
            if (cmds_.front().find('|') != string::npos || cmds_.front().find('!') != string::npos) // normal & error pipe
            {
                int pipe_num;
                char pipe_char[5];
                
                if (cmds_.front().length() == 1) // simple pipe，cmds_ 還未 pop 出來
                {
                    pipe_num = 1;
                } 
                else // numbered-pipe
                {
                    for (int i = 1; i < (int)cmds_.front().length(); ++i) // chars to int
                    {
                        pipe_char[i-1] = cmds_.front()[i];
                        pipe_char[i] = '\0';
                    }
                    pipe_num = atoi(pipe_char);
                }

                // 知道動作，且現在的 out_fd，就是 pipe 的 in_fd 一定是後面那個參數
                // 獲得 out_fd 
                CreatePipe(pipe_vector_, pipe_num, out_fd); 

                if (cmds_.front().find('!') != string::npos) 
                {
                    err_fd = out_fd;
                }

                is_first_argv = true;
                is_final_argv = true;
                is_using_pipe = true;
                if (cmds_.empty()) 
                {
                    line_ends = true;
                }
                cmds_.pop(); // 符號 pop 掉
            } 
            else if (cmds_.front() == ">" || cmds_.front() == "<") // redirection > & <
            {
                string op = cmds_.front();
                cmds_.pop(); // 將指令 pop 出去

                int file_fd;
                if (op == ">") 
                {
                    file_fd = open(cmds_.front().c_str(), O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR); 
                } 
                else // op == "<"
                {
                    file_fd = open(cmds_.front().c_str(), O_RDONLY, 0644);
                }

                if (file_fd < 0) 
                {
                    cerr << "open file error" << endl;
                }
                cmds_.pop(); // 將 destination file pop 出去，因為 source file 已經在前面解析過了

                if (op == ">") 
                {
                    out_fd = file_fd;
                } 
                else // op == "<"
                {
                    in_fd = file_fd;
                }
                
                is_using_pipe = false;
                is_in_redirect = true;
                if (cmds_.empty()) 
                {
                    line_ends = true;
                    is_first_argv = true;
                    is_final_argv = true;
                }
            } 
            else if ((cmds_.front().find('>') != string::npos) && (cmds_.front() != ">")) // named pipe (out), ex: >2
            {
                char user_char[3];

                for (int i = 1; i < (int)cmds_.front().length(); ++i) 
                {
                    user_char[i-1] = cmds_.front()[i];
                    user_char[i] = '\0';
                }
                
                target_id = atoi(user_char);
                Client_info* shm_cli;

                if (GetClientByID(target_id, shm_cli) == NULL)  // target id not exist
                {
                    cmds_.pop();
                    
                    cout << "*** Error: user #" << target_id << " does not exist yet. ***" << endl;
                    target_id = -1;
                    
                    queue<string> empty;
                    swap(cmds_, empty); // 後面是啥都不重要了
                    return 0;
                } 
                else   // target id exists
                {
                    // check if user pipe already exists
                    FIFOInfo* shm_fifo = GetFIFOSHM(g_shmid_fifo);
                    if (shm_fifo->fifo[cur_id-1][target_id-1].in_fd != -1) // 雖然需要檢查 in_fd & out_fd 但主要是靠[cur_id -1 ][target_id]
                    {
                        cout << "*** Error: the pipe #" << cur_id << "->#" << target_id;
                        cout << " already exists. ***" << endl;
                        
                        queue<string> empty;
                        swap(cmds_, empty);
                        return 0;
                    }
                    shmdt(shm_fifo);

                    // mkFIFO and record FIFOname
                    // 寫入 fifo 是 execcmd 的部分
                    CreateUserPipe(cur_id, target_id, out_fd);
                    is_using_pipe = true;
                    is_in_userpipe = true;
                    cmds_.pop();
                    if (cmds_.empty()) 
                    {
                        line_ends = true;
                        is_first_argv = true;
                        is_final_argv = true;
                    }
                    send_str_id = target_id;
                }
            } 
            else if ((cmds_.front().find('<') != string::npos) && (cmds_.front() != "<")) // named pipe (in), ex: <2
            {
                char user_char[3];

                for (int i = 1; i < (int)cmds_.front().length(); ++i) 
                {
                    user_char[i-1] = cmds_.front()[i];
                    user_char[i] = '\0';
                }

                source_id = atoi(user_char);
                Client_info* shm_cli;
                
                if (GetClientByID(source_id, shm_cli) == NULL) // target id does not exist
                {
                    cmds_.pop();

                    cout << "*** Error: user #" << source_id << " does not exist yet. ***" << endl;
                    source_id = -1;
                    
                    queue<string> empty;
                    swap(cmds_, empty);
                    return 0;
                } 
                else // target id exists
                {
                    // check if user pipe already exists
                    FIFOInfo* shm_fifo = GetFIFOSHM(g_shmid_fifo);
                    if (shm_fifo->fifo[source_id-1][cur_id-1].out_fd == -1) 
                    {
                        // cannot find any userpipe's target id is current client id
                        cout << "*** Error: the pipe #" << source_id << "->#" << cur_id;
                        cout << " does not exist yet. ***" << endl;
                        
                        queue<string> empty;
                        swap(cmds_, empty);
                        return 0;
                    }
                    shmdt(shm_fifo);

                    GetUserPipeFd(source_id, cur_id, in_fd);
                    is_in_userpipe = true;
                    cmds_.pop();
                    if (cmds_.empty()) 
                    {
                        line_ends = true;
                        is_first_argv = true;
                        is_final_argv = true;
                    }
                    recv_str_id = source_id;
                }
            } 
            else // cat test.html, number test.html, removetag test.html
            {
                arguments.push_back(cmds_.front());
                cmds_.pop();
                is_final_argv = IsQueueEmpty(cmds_);
                is_using_pipe = false;
                if (cmds_.empty()) 
                {
                    line_ends = true;
                }
            }
        } 
        
        // 預設非最後指令，例如有，exit,yell,tell 則直接符合
        // 同時也是真正 execute 的地方
        if (is_final_argv) 
        {
            // broadcast send and recv
            if (recv_str_id != -1) 
            {
                string line_input = terminal_input_.substr(0, terminal_input_.length());
                if (line_input.back()=='\r') 
                {
                    line_input.pop_back();
                }
                recv_str_id = -1;
                Broadcast("recv", line_input, cur_id, source_id);
                usleep(500);
            }
            if (send_str_id != -1) 
            {
                string line_input = terminal_input_.substr(0, terminal_input_.length());
                if (line_input.back()=='\r') 
                {
                    line_input.pop_back();
                }
                send_str_id = -1;
                Broadcast("send", line_input, cur_id, target_id);
                usleep(500);
            }
            
            // pipe: get pipe (count == 0)
            // 是否有需要結束的 pipe 做處理
            bool need_close_pipe = GetPipeFd(pipe_vector_, in_fd);

            // execute
            // 若是 yell, tell 會把後面所有東西放入 arguments
            // 並且也是等所有東西都備齊後，才去真正執行
            // 也因此會在 is_final_argv 執行
            // 分出 child 和 parent
            // 若是 build-in 指令會相當方便，直接跳到 is_final_argv
            status = ExecCmd(arguments, IsExecutable(prog, path_vector_), in_fd, out_fd, err_fd, line_ends, is_using_pipe, target_id);

            // execcmd 已經有做完事情了，pipe 可以消除，並做 countdown
            // need_close_pipe 是從 process 這邊關掉自己的 fd，而 erasedpipefd 則是從 pipe_vector 那邊去關閉
            ErasePipefd(pipe_vector_);
            CountdownPipefd(pipe_vector_);

            // 別人將資料進到我這邊，視為我的 in_fd，
            // 所以要關閉當然是關閉 pipe 的輸出，也就是我的輸入
            if (need_close_pipe) 
            {
                close(in_fd);
            }
            if (target_id > 0) {
                target_id = -1;
            }

            // if userpipe in , then erase
            // 代表由從其他地方 source 接受到 userpipe 的資訊
            // 這裡已經是執行完程式了
            // 當然會需要關掉 userpipe 啦
            if (source_id > 0) {
                EraseUserPipe(source_id);
                source_id = -1;
            }

            is_final_argv = false;
            is_in_redirect = false;
            is_in_userpipe = false;
        }
    }
    return status;
}

// 或許用不著成為一個函式，但會方便許多
bool Shell::IsQueueEmpty(queue<string> cmds) {

    if (cmds.empty()) return true;
    return false;

}

// 某些參數由哪些指令影響，還需釐清
int Shell::ExecCmd(vector<string> &arguments, bool is_executable, int in_fd, int out_fd, int err_fd, bool line_ends, bool is_using_pipe, int target_id) {

    // args 做操作，arguments 其實不會被操作到
    char *args[arguments.size() + 1];
    for (size_t i = 0; i < arguments.size(); ++i) 
    {
        args[i] = new char[arguments[i].size() + 1];
        strcpy(args[i], arguments[i].c_str());
    }

    // built-in
    // 將 char array args[0] 弄為 string prog
    string prog(args[0]);
    if (prog == "printenv") 
    {
        PrintEnv(args[1]);
        return 0;
    } 
    else if (prog == "setenv") 
    {
        SetEnv(args[1], args[2]);
        return 0;
    } 
    else if (prog == "who") 
    {
        Who();
        return 0;
    } 
    else if (prog == "yell") 
    {
        string msg = "";

        for (size_t i = 1; i < arguments.size(); ++i) //args[0] 為指令，因此從 1 開始
        {
            msg += string(args[i]) + " ";
        }

        msg.pop_back(); //最後的空格
        Yell(msg);
        return 0;
    } 
    else if (prog == "tell") 
    {
        string msg = "";

        for (size_t i = 2; i < arguments.size(); ++i) // 參數 2 為 receiver 
        {
            msg += string(args[i]) + " ";
        }

        msg.pop_back();
        Tell(stoi(args[1]), msg); //已經在 cur_id 因此只需接受者的資訊
        return 0;
    } 
    else if (prog == "name") 
    {
        Name(string(args[1])); 
        return 0;
    } 
    else if (prog == "exit") {
        return -1; 
        // shell return -1 ，代表結束
        // shell return 其他則沒事，繼續接受 input
    }

    // not built-in
    signal(SIGCHLD, ChildHandler);// 如果 child 先完成，則需要先等 parent，使用 waitpid
    pid_t child_pid;
    child_pid = fork();
    while (child_pid < 0) {
        usleep(1000);
        child_pid = fork();
    }

    // slave_child process
    if (child_pid == 0) 
    {
        // 有東西要送，因此需要對 stdoutfd 做修改，current client open FIFO and record write fd
        if (target_id > 0) 
        {
            SetUserPipeOut(target_id, out_fd);
        }

        // pipe ops
        ConnectPipeFd(in_fd, out_fd, err_fd);
        if (!is_executable) 
        {
            cerr << "Unknown command: [" << args[0] << "]." << endl;
            exit(0);
        } 
        else 
        {
            args[arguments.size()] = NULL;
            if(execvp(args[0], args) < 0) 
            {
                cerr << "execl error" << endl;
                exit(0);
            }
            cerr << "execvp" << endl;
            exit(0);
        }
    } 
    else  // slave_parent process
    {
        if (line_ends) // 執行前傳過來的,某些情況會使得執行時不為最後，例如
        {
            if (!is_using_pipe) // 也沒有 使用 pipe 
            {
                int status;
                waitpid(child_pid, &status, 0);
            }
        }
    }
    return 0;
}

// 可否執行，直接是內建指令，或是可以由路徑找到執行檔
bool Shell::IsExecutable(string prog, vector<string> &path_vector_) {

    if (prog == "printenv" || prog == "setenv" || prog == "exit" ||
        prog == "who" || prog == "tell" || prog == "yell" || prog == "name") {
        return true;
    }

    // 搜尋可執行檔
    bool is_executable;
    string path;
    vector<string>::iterator iter = path_vector_.begin();
    while (iter != path_vector_.end()) {
        path = *iter;
        path = path + "/" + prog;
        is_executable = (access(path.c_str(), 0) == 0);
        if (is_executable) {
            return true;
        }
        ++iter;
    }
    return false;
}

// 主要判斷有沒有人需要共用 pipe
// 不然就新建一個
void Shell::CreatePipe(vector<PipeFd> &pipe_vector_, int pipe_num, int &in_fd) {

    // check if pipe to same pipe
    // has same pipe => reuse old pipe (multiple write, one read)
    // no same pipe => create new pipe (one write, one read)
    bool has_same_pipe = false;

    vector<PipeFd>::iterator iter = pipe_vector_.begin();
    while(iter != pipe_vector_.end()) 
    {
        if ((*iter).count == pipe_num)  // 遇到同樣要寫入別地方的 number pipe
        {
            has_same_pipe = true;
            in_fd = (*iter).in_fd;
        }
        ++iter;
    }

    if (has_same_pipe) //同樣管子，就不用再新建了
    {
        return;
    }

    // create pipe
    int pipe_fd[2];
    if (pipe(pipe_fd) < 0) 
    {
        cerr << "pipe error" << endl;
        exit(0);
    }
    
    PipeFd new_pipefd;
    new_pipefd.in_fd = pipe_fd[1];  // write fd
    new_pipefd.out_fd = pipe_fd[0];  // read fd
    new_pipefd.count = pipe_num;
    pipe_vector_.push_back(new_pipefd);

    // 現在這個 process 的 out_fd，就是這裡要寫入 pipe 的 write_fd
    // 也就是將 process 的 out_fd 改寫成獲得的 pipe_fd
    in_fd = pipe_fd[1]; 
}

// 將 pipefd 之 count 做 countdown
void Shell::CountdownPipefd(vector<PipeFd> &pipe_vector_) {

    vector<PipeFd>::iterator iter = pipe_vector_.begin();
    while (iter != pipe_vector_.end()) 
    {
        --(*iter).count;
        ++iter;
    }
}

// 解除一般 pipe 的資訊
void Shell::ErasePipefd(vector<PipeFd> &pipe_vector_) {

    vector<PipeFd>::iterator iter = pipe_vector_.begin();
    while (iter != pipe_vector_.end()) {
        if ((*iter).count == 0) 
        {
            close((*iter).in_fd);
            close((*iter).out_fd);
            pipe_vector_.erase(iter);
        } 
        else 
        {
            ++iter;
        }
    }
}

// 判斷是否有到期的 pipe
bool Shell::GetPipeFd(vector<PipeFd> &pipe_vector_, int& in_fd) {

    vector<PipeFd>::iterator iter = pipe_vector_.begin();
    while (iter != pipe_vector_.end()) 
    {
        if ((*iter).count == 0)  // *iter 為 vector 中其中一份 pipefd
        {
            close((*iter).in_fd);
            in_fd = (*iter).out_fd; // ?
            return true;
        }
        ++iter;
    }
    return false;
}

// 因為 fd 都設置好了，
// 只需要將 stdIOfd 改為 我們要的 fd
void Shell::ConnectPipeFd(int in_fd, int out_fd, int err_fd) {

    if (in_fd != STDIN_FILENO) {
        dup2(in_fd, STDIN_FILENO);
    }
    if (out_fd != STDOUT_FILENO) {
        dup2(out_fd, STDOUT_FILENO);
    }
    if (err_fd != STDERR_FILENO) {
        dup2(err_fd, STDERR_FILENO);
    }
    if (in_fd != STDIN_FILENO) {
        close(in_fd);
    }
    if (out_fd != STDOUT_FILENO) {
        close(out_fd);
    }
    if (err_fd != STDERR_FILENO) {
        close(err_fd);
    }
}

// 小孩需等父母回收
void Shell::ChildHandler(int signo) {
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0) {}
}

// 使用 mkfifio 建立，並立即通知 target id 接受
void Shell::CreateUserPipe(int cur_id, int target_id, int &out_fd) {
    
    char fifopath[MAX_PATH_SIZE];
    sprintf(fifopath, "%suser_pipe_%d_%d", FIFO_PATH, cur_id, target_id);
    
    if (mkfifo(fifopath, 0666 | S_IFIFO) < 0) 
    {
        cerr << "mkfifo error" << endl;
        exit(0);
    }

    Client_info* shm_cli = GetCliSHM(g_shmid_cli);
    Client_info* target_cli = GetClientByID(target_id, shm_cli);
    FIFOInfo* shm_fifo = GetFIFOSHM(g_shmid_fifo);
    strncpy(shm_fifo->fifo[cur_id-1][target_id-1].name, fifopath, MAX_PATH_SIZE);

    // signal target client to open fifo and read
    kill(target_cli->pid, SIGUSR2);
    shmdt(shm_cli);
    shmdt(shm_fifo);
}

// ?
// 設定 user_pipe，這次是額外將 user_pipe 設定在外面資料夾
// 因為讀取指令時，當然知道當下的
// 這裡的 target_id 是 userpipe_target_id
// create 的時候沒有設
void Shell::SetUserPipeOut(int target_id, int& out_fd) {

    char fifopath[MAX_PATH_SIZE];
    sprintf(fifopath, "%suser_pipe_%d_%d", FIFO_PATH, cur_id, target_id);

    Client_info* shm_cli = GetCliSHM(g_shmid_cli);
    FIFOInfo* shm_fifo = GetFIFOSHM(g_shmid_fifo);

    out_fd = open(fifopath, O_WRONLY);
    shm_fifo->fifo[cur_id-1][target_id-1].in_fd = out_fd; // 現在這個程序的 input 就是接受由

    shmdt(shm_cli);
    shmdt(shm_fifo);
}

// 只是獲得 id，並未真正從 fifo 讀取
// 真正讀取是從 execcmd 那邊
void Shell::GetUserPipeFd(int source_id, int cur_id, int& in_fd) {
    
    char fifopath[MAX_PATH_SIZE];
    sprintf(fifopath, "%suser_pipe_%d_%d", FIFO_PATH, source_id, cur_id);
    Client_info* shm_cli = GetCliSHM(g_shmid_cli);
    FIFOInfo* shm_fifo = GetFIFOSHM(g_shmid_fifo);

    shm_fifo->fifo[source_id-1][cur_id-1].in_fd = -1;
    in_fd = shm_fifo->fifo[source_id-1][cur_id-1].out_fd;
    shm_fifo->fifo[source_id-1][cur_id-1].is_used = true;

    shmdt(shm_cli);
    shmdt(shm_fifo);
}

// 使用完 user_pipe
void Shell::EraseUserPipe(int id) {

    FIFOInfo* shm_fifo = GetFIFOSHM(g_shmid_fifo);

    if (shm_fifo->fifo[id-1][cur_id-1].is_used) 
    {
        shm_fifo->fifo[id-1][cur_id-1].in_fd = -1;
        shm_fifo->fifo[id-1][cur_id-1].out_fd = -1;
        shm_fifo->fifo[id-1][cur_id-1].is_used = false;

        unlink(shm_fifo->fifo[id-1][cur_id-1].name);
        memset(&shm_fifo->fifo[id-1][cur_id-1].name, 0, sizeof(shm_fifo->fifo[id-1][cur_id-1].name));
    }
}

// 建立 tcp server 
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

// 可以直接使用 cout，因為環境已經限制在個別 slave 了
void PrintWelcome() {
    string msg = "";
    msg += "****************************************\n";
    msg += "** Welcome to the information server. **\n";
    msg += "****************************************\n";
    cout << msg;
}

// target_id 為 -1 代表
void Broadcast(string action, string msg, int cur_id, int target_id) {
    string send_msg = "";
    Client_info* shm_cli;
    Client_info* cur_cli = GetClientByID(cur_id, shm_cli);
    Client_info* target_cli; // 雖是 broadcast 還需要 target_id 是因為 gui 顯示的介面需要

    if (target_id != -1) {
        target_cli = GetClientByID(target_id, shm_cli);
    } else {
        target_cli = NULL;
    }

    if (cur_cli == NULL) {
        cout << "cannot find client id: " << cur_id << endl;
        return;
    }

    if (action == "login") {
        send_msg = "*** User '(no name)' entered from " + string(cur_cli->user_ip) + ":" + to_string(cur_cli->port) + ". ***\n";
    } else if (action == "logout") {
        send_msg = "*** User '" + string(cur_cli->user_name) + "' left. ***\n";
    } else if (action == "name") {
        send_msg = "*** User from " + string(cur_cli->user_ip) + ":" + to_string(cur_cli->port);
        send_msg += " is named '" + string(cur_cli->user_name) + "'. ***\n";
    } else if (action == "yell") {
        send_msg = "*** " + string(cur_cli->user_name) + " yelled ***: " + msg + "\n";   
    } else if (action == "send") {
        send_msg = "*** " + string(cur_cli->user_name) + " (#" + to_string(cur_cli->id) + ") just piped '";
        send_msg += msg + "' to " + string(target_cli->user_name) + " (#" + to_string(target_cli->id) + ") ***\n";
    } else if (action == "recv") {
        send_msg = "*** " + string(cur_cli->user_name) + " (#" + to_string(cur_cli->id);
        send_msg += ") just received from " + string(target_cli->user_name) + " (#";
        send_msg += to_string(target_cli->id) + ") by '" + msg + "' ***\n";
    }

    // 將 msg 放到 shm 中，i.e. 透過 shm 傳遞
    char* shm_msg = GetMsgSHM(g_shmid_msg);
    sprintf(shm_msg, "%s", send_msg.c_str());
    shmdt(shm_msg);

    usleep(500);

    // 呼叫其他使用者，使用中斷方式，並用 clishm 去獲得使用者資訊
    shm_cli = GetCliSHM(g_shmid_cli);
    for (int i = 0; i < MAX_USER; ++i) {
		if (shm_cli[i].valid == 1) {
            kill(shm_cli[i].pid, SIGUSR1);
        }
	}

    // 斷開連結
	shmdt(shm_cli);
    shmdt(shm_msg);
}

// 需要傳送訊息到個別，
// 因此需要 msg_shm cli_shm
void BroadcastOne(string action, string msg, int cur_id, int target_id) {
    string send_msg = "";
    Client_info* shm = GetCliSHM(g_shmid_cli);
    Client_info* cur_cli = GetClientByID(cur_id, shm);

    // broadcast to one
    if (action == "tell") {
        send_msg = "*** " + string(cur_cli->user_name) + " told you ***: " + msg + "\n";
    }

    char* shm_msg = GetMsgSHM(g_shmid_msg);
    sprintf(shm_msg, "%s", send_msg.c_str());
    shmdt(shm_msg);
    usleep(500);

    shm = GetCliSHM(g_shmid_cli);
    kill(shm[target_id-1].pid, SIGUSR1); // 發送訊息的中斷訊號給目標
    shmdt(shm);
}

// 只是將所有 client_info 弄成 0 
void ResetCliSHM(int g_shmid_cli) {

    // shmat 將區段與 process 連接
    //注意區域變數與全域變數同名，以區域變數為主
	Client_info *shm = (Client_info*)shmat(g_shmid_cli, NULL, 0);
	if (shm < (Client_info*)0) {
		cerr << "Error: shmat() failed" << endl;
		exit(1);
	}
    // shm 為指標，array 上的值，因此 shm[i] 不為 pointer
	for (int i = 0; i < MAX_USER; ++i) {
		shm[i].valid = 0;
	}
    // 將內存 shm 與 process 分離
	shmdt(shm); 
}

// 將 fifo 上的 fd 等等資訊刪除
void ResetFIFOSHM(int g_shmid_fifo) {

	FIFOInfo *shm_fifo = GetFIFOSHM(g_shmid_fifo);
	for (int i = 0; i < MAX_USER; ++i) {
        for (int j = 0; j < MAX_USER; ++j) {

            // shm_fifo 為指標，因此存取時需要使用 ->
            shm_fifo->fifo[i][j].in_fd = -1;
            shm_fifo->fifo[i][j].out_fd = -1;
            shm_fifo->fifo[i][j].is_used = 0;
            char name[MAX_PATH_SIZE];
            memset(&shm_fifo->fifo[i][j].name, 0, sizeof(name));
        }
	}
	shmdt(shm_fifo);
}

// 初始話 shm 並且將其 pointer 設定為 global
// 並且賦予位置代號，方便存取，i.e. g_shmid_xxx 
void Init_SHM() {

	int shm_size = sizeof(Client_info) * MAX_USER;
	int msg_size = sizeof(char) * MAX_MSG_LEN;
    int fifo_size = sizeof(FIFOInfo);

    // shmget 創建共享區段(區段名稱，區段大小，使用者權限與使用此區段模式)
    int shmid_cli = shmget(SHM_KEY, shm_size, 0666 | IPC_CREAT);
	if (shmid_cli < 0) {
		cerr << "Error: init_shm() failed" << endl;
		exit(1);
	}
    int shmid_msg = shmget(SHM_MSG_KEY, msg_size, 0666 | IPC_CREAT);
	if (shmid_msg < 0) {
		cerr << "Error: init_shm() failed" << endl;
		exit(1);
	}
    int shmid_fifo = shmget(SHM_FIFO_KEY, fifo_size, 0666 | IPC_CREAT);
	if (shmid_fifo < 0) {
		cerr << "Error: init_shm() failed" <<endl;
		exit(1);
	}

	ResetCliSHM(shmid_cli);
    ResetFIFOSHM(shmid_fifo);

	// update global var
	g_shmid_cli = shmid_cli;
	g_shmid_msg = shmid_msg;
    g_shmid_fifo = shmid_fifo;
}

// 存取指定 id 的 shm 位置
Client_info* GetCliSHM(int g_shmid_cli) {
    Client_info* shm = (Client_info*)shmat(g_shmid_cli, NULL, 0);
	if (shm < (Client_info*)0) {
		cerr << "Error: get_new_id() failed" << endl;
		exit(1);
	}
    return shm;
}

// attach 到 msg_shm
char* GetMsgSHM(int g_shmid_msg) {
    char* shm = (char*)shmat(g_shmid_msg, NULL, 0);
	if (shm < (char*)0) {
		cerr << "Error: get_new_id() failed" << endl;
		exit(1);
	}
    return shm;
}

// get_fifo_new_id
FIFOInfo* GetFIFOSHM(int g_shmid_fifo) {
    FIFOInfo* shm = (FIFOInfo*)shmat(g_shmid_fifo, NULL, 0);
	if (shm < (FIFOInfo*)0) {
		cerr << "Error: get_new_id() failed" << endl;
		exit(1);
	}
    return shm;
}

// get available ID
// 結束對 shm 的操作，需要斷開連結
// 獲得目前 cli 人數（存在 shm 內）
// 1 代表有人佔用，0 代表無人使用
int GetIDFromSHM() {

    // attach to cli_shm 去做操作
	Client_info *shm = GetCliSHM(g_shmid_cli);
	for (int i = 0; i < MAX_USER; ++i) {
		if (!shm[i].valid) {
            shm[i].valid = 1; 
			shmdt(shm);
			return (i+1);
		}
	}
	shmdt(shm); // 都有人使用，只能回報錯誤
    return -1;
}

// 先 attach 到 cli_shm 中
// id 一樣且有人佔用，就可以回傳 cli_info
Client_info* GetClientByID(int id, Client_info* shm_cli) {

    shm_cli = GetCliSHM(g_shmid_cli);
	for (int i = 0; i < MAX_USER; ++i) 
    {
		if ((shm_cli[i].id == id) && (shm_cli[i].valid == 1)) 
        {
            Client_info* res = &shm_cli[i];
			return res;
		}
	}
    return NULL;
}

// fifo 那邊怪怪
void SigHandler(int sig) {

	if (sig == SIGUSR1) // 自定義之中斷訊號 receive messages from others
    {
		char* msg = (char*)shmat(g_shmid_msg, NULL, 0);
        if (msg < (char*)0) {
            cerr << "Error: shmat() failed" << endl;
            exit(1);
        }
        if (write(STDOUT_FILENO, msg, strlen(msg)) < 0) {
            cerr << "Error: broadcast_catch() failed" << endl;
        }

        // 斷開與 msg_shm 的連結
        shmdt(msg);
    
	} 
    else if (sig == SIGUSR2) // 自定義之中斷訊號 open FIFOs to read
    {
        FIFOInfo* shm_fifo = GetFIFOSHM(g_shmid_fifo);
		int	i;
		for (i = 0; i < MAX_USER; ++i) 
        {
			if (shm_fifo->fifo[i][cur_id-1].out_fd == -1 && shm_fifo->fifo[i][cur_id-1].name[0] != 0) // 不管從何者來的 fifo 都給我 且是要有名字的
            {
                shm_fifo->fifo[i][cur_id-1].out_fd = open(shm_fifo->fifo[i][cur_id-1].name, O_RDONLY); // 雖然接收到中斷，但要主動 <2 才會真正成為 out_fd
            }
		}
        shmdt(shm_fifo);
	} 
    else if (sig == SIGINT || sig == SIGQUIT || sig == SIGTERM)     // clean a client 因為是在 shell 內做 handle，主要是清除 fifo
    {
        Broadcast("logout", "", cur_id, -1);
        FIFOInfo* shm_fifo = GetFIFOSHM(g_shmid_fifo);
        for (size_t i = 0; i < MAX_USER; ++i) {
            if (shm_fifo->fifo[i][cur_id-1].out_fd != -1) {
                // read out message in the unused fifo
                char buf[1024];
                while (read(shm_fifo->fifo[i][cur_id-1].out_fd, &buf, sizeof(buf)) > 0) {}
                shm_fifo->fifo[i][cur_id-1].out_fd = -1;
                shm_fifo->fifo[i][cur_id-1].in_fd = -1;
                shm_fifo->fifo[i][cur_id-1].is_used = false;
                unlink(shm_fifo->fifo[i][cur_id-1].name);
                memset(shm_fifo->fifo[i][cur_id-1].name, 0, sizeof(shm_fifo->fifo[cur_id-1][i].name));
            }
        }
	}
	signal(sig, SigHandler);
}

// 將 new_user 加入到 shm 內部
void AddClient(int id, int sockfd, sockaddr_in address) {
	Client_info* shm;
    int shm_idx = id - 1;
	if (id < 0) {
		cerr << "Error: get_new_id() failed" << endl;
        exit(1);
	}
    shm = (Client_info*)shmat(g_shmid_cli, NULL, 0);
	if (shm < (Client_info*)0) {
		cerr << "Error: init_new_client() failed" << endl;
        exit(1);
	}
	shm[shm_idx].valid = 1; // 1 表示佔用
    shm[shm_idx].id = id;
	shm[shm_idx].pid = getpid();
	shm[shm_idx].port = ntohs(address.sin_port);
    strncpy(shm[shm_idx].user_ip, inet_ntoa(address.sin_addr), INET_ADDRSTRLEN);
	strcpy(shm[shm_idx].user_name, "(no name)");

    // 斷開連結
	shmdt(shm); 
}

// 真正刪除 shm 內部的所以資料
void DelSHM() {
	// delete all
	shmctl(g_shmid_cli, IPC_RMID, NULL);
	shmctl(g_shmid_msg, IPC_RMID, NULL);
    shmctl(g_shmid_fifo, IPC_RMID, NULL);
}

// server 按下 ctrl+c 時的中斷處理
void Server_sig_handler (int sig){
	if (sig == SIGCHLD) {
		while (waitpid (-1, NULL, WNOHANG) > 0);
	} else if(sig == SIGINT || sig == SIGQUIT || sig == SIGTERM) {
		DelSHM(); //若是結束程式之中斷訊號，則需要清除 shm
		exit (0);
	}
	signal (sig, Server_sig_handler); //?
}

int main(int argc, char* argv[]) {
    setenv("PATH", "bin:.", 1);
    if (argc != 2) {
        cerr << "./np_multi_proc [port]" << endl;
        exit(1);
    }

    int ser_sockfd;
    int cli_sockfd;
    int slave_pid;
    socklen_t addr_len;
    struct sockaddr_in cli_addr;

    int port = atoi(argv[1]);
    ser_sockfd = TCP_establish(port);
    cout << "Server sockfd: " << ser_sockfd << " port: " << port << endl;

    // 對 server 極為重要，因為只有 ctrl＋c 可以中斷，並清除 shm
    signal (SIGCHLD, Server_sig_handler);
	signal (SIGINT, Server_sig_handler);
	signal (SIGQUIT, Server_sig_handler);
	signal (SIGTERM, Server_sig_handler);

    Init_SHM();

    while (1) {
        addr_len = sizeof(cli_addr);
        cli_sockfd = accept(ser_sockfd, (struct sockaddr *) &cli_addr, &addr_len);
        if (cli_sockfd < 0) {
            cerr << "Error: accept failed" << endl;
            continue;
        }
        cout << "client sockfd: " << cli_sockfd << endl;

        slave_pid = fork();
        while (slave_pid < 0) {
            slave_pid = fork();
        }
        if (slave_pid == 0) //slave 去接待客人喔
        {
            // 將 cli_sockfd 全部寫入 fd table 內
            // 之後區域內遇到的 cout/printf 都會輸入輸出到 cli_sockfd
            dup2(cli_sockfd, STDIN_FILENO);
            dup2(cli_sockfd, STDOUT_FILENO);
            dup2(cli_sockfd, STDERR_FILENO);

            // 用不到的檔案描述符，就要 close 掉
            close(cli_sockfd);
            close(ser_sockfd);

            // 之前的 user_info 需要改存在 shm
            int cli_id = GetIDFromSHM();
            AddClient(cli_id, cli_sockfd, cli_addr);

            // 前面幫客人建立資料，現在真正去服務 client 了
            // 目前只有先宣告，真正執行在 shell.clientexec
            Shell shell;

            // 每個 client 透過此函數去執行，當回傳結束時
            // 就會往下去完成 clean cli_info 與 clean fifo
            // cli_id 與 cur_id 之不同 ？？ 有沒有同步？
            if (shell.ClientExec(cli_id) == -1) // exec 中，若回傳 -1，則代表 cli 指令 exit
            { 
                Broadcast("logout", "", cur_id, -1); 

                // clean cli＿info
                Client_info* shm_cli = GetCliSHM(g_shmid_cli);
                shm_cli[cur_id-1].valid = 0;
                shmdt(shm_cli); //區塊斷開共享記憶體連線

                // clean FIFO＿info，不過照理講， clear fifo 應該會在 shell 內的 sighandler 就解決的，可能是保險起見
                FIFOInfo* shm_fifo = GetFIFOSHM(g_shmid_fifo);
                for (size_t i = 0; i < MAX_USER; i++) {
                    if (shm_fifo->fifo[i][cur_id-1].out_fd != -1) {

                        // 將 unused fifo 的東西吐出來
                        char buf[1024]={};
                        while (read(shm_fifo->fifo[i][cur_id-1].out_fd, &buf, sizeof(buf)) > 0){}
                        shm_fifo->fifo[i][cur_id-1].out_fd = -1;
                        shm_fifo->fifo[i][cur_id-1].in_fd = -1;
                        shm_fifo->fifo[i][cur_id-1].is_used = false;

                        // 因為 FIFO 為檔案路徑，因此用 .name 並且是使用 FIFO 特有的 unlink
                        unlink(shm_fifo->fifo[i][cur_id-1].name);
                        memset(shm_fifo->fifo[i][cur_id-1].name, 0, sizeof(shm_fifo->fifo[cur_id-1][i].name));
                    }
                }
                // 將 fifo 所佔用的 shm 空間與 process 斷開連結
                shmdt(shm_fifo); 

                close(STDIN_FILENO);
                close(STDOUT_FILENO);
                close(STDERR_FILENO);
                exit(0);
            }
        } 
        else //總機關閉與客人之對話，回到櫃檯
        {
            close(cli_sockfd);
        }
    }
    close(ser_sockfd);
}