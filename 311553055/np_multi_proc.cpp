#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <cstring>
#include <string.h>
#include <ctype.h>
#include <vector>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sstream>
#include <unistd.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/shm.h>

using namespace std;

#define SHM_Users_KEYS 10701
#define SHM_Broadcast_Msg_KEYS 10702

const size_t CMD_BUFFER=15001;

class User{
	public:
		bool has_User = 0;
		int pid = 0;
		char address[30];
		char name[55];
};

class BroadcastMsg{
	public:
		char str[1000];
};

struct PipeInfo{
    int fd[2];     // 0 = read, 1 = write
    int count;
};

User *users;
BroadcastMsg *msg;
int ID;
int shm_1, shm_2;


bool checkExist(int srcID, int tarID)
{
    char fifo[20];
    sprintf(fifo,"user_pipe/%d_%d", srcID, tarID);
    if(access(fifo, F_OK) != -1)
        return 1;
    return 0;
}

void broadcast(int *sourceID, int *targetID, char *m)
{
	strcpy(msg->str, m);
	if (targetID == NULL)
	{
		for (int i=0; i<30; i++)
		{
			if (users[i].has_User == 1)
				kill(users[i].pid, SIGUSR1);
		}
	} 
	else 
		kill(users[*targetID].pid, SIGUSR1);
}

void who()
{
	printf("<ID>\t<nickname>\t<IP:port>\t<indicate me>\n");
	for (int i=0; i<30; i++)
	{
		if (users[i].has_User == 1)
		{
			printf("%d\t%s\t%s", i+1, users[i].name, users[i].address);
			if (i == ID)
				printf("\t<-me");
			printf("\n");
		}
	}
}
		
void tell(int targetID, char *s)
{ 
	
	char m[5000];
	
	if (users[--targetID].has_User == 1)
	{
		sprintf(m, "*** %s told you ***: %s\n", users[ID].name, s);
		broadcast(NULL, &targetID, m);
	} 
	else
		printf("*** Error: user #%d does not exist yet. ***\n", targetID+1);
}

void yell(char *s) //Good morning everyone.
{
	char msg[5000];
	sprintf(msg, "*** %s yelled ***: %s\n", users[ID].name, s);
	broadcast(NULL, NULL, msg);
}

void name(char *newName)
{ 

	char m[1000];

	for (int j=0; j<30; j++)
	{
		if (j == ID)
			continue;
		if (users[j].has_User == 1 && (strcmp(users[j].name,newName) == 0))
		{
			sprintf(m, "*** User '%s' already exists. ***\n", newName);
			broadcast(NULL, &ID, m);
			return;
		}
	}

	strcpy(users[ID].name, newName);
	sprintf(m, "*** User from %s is named '%s'. ***\n", users[ID].address, users[ID].name);
	broadcast(NULL, NULL, m);
}

void Exec(vector<string> argus)
{
	vector<const char*> ptrVec;

    for(int i=0; i<argus.size(); i++)
	{
        ptrVec.push_back(argus[i].c_str());
    }
	ptrVec.push_back(NULL);
	const char** commands = &ptrVec[0];
    
    if(execvp(argus[0].c_str(),(char* const*)commands) < 0)
	{
    	fprintf(stderr,"Unknown command: [%s].\n",argus[0].c_str());
        exit(1);
    }
}

void SignalHandler(int signal)
{
	printf("%s", msg->str);
}

void SigForkHandler(int signal)
{
	int status;
	while(waitpid(-1,&status,WNOHANG) > 0){}
}

void np_shell()
{
	signal(SIGCHLD, SigForkHandler);
    signal(SIGUSR1, SignalHandler);
	clearenv();
	
	char w[] = "****************************************\n** Welcome to the information server. **\n****************************************";
	cout<<w<<endl;
	
	string log = "*** User '"+string(users[ID].name)+"' entered from "+string(users[ID].address)+". ***\n";
	broadcast(NULL, NULL, strdup(log.c_str()));
	setenv("PATH", "bin:.", 1);
    vector <PipeInfo> numPipes;

    while(1)
	{
		char *input = (char*)malloc(CMD_BUFFER);
		char in[CMD_BUFFER];
		char intact[CMD_BUFFER];
		vector<vector<string>> cmds;

		bool FileFlag = 0;

		int PipeType = -1;     // -1 normal pipe, 0 number pipe, 1 error number pipe
		int PipeCount = 0;

		int pipeToID = 0;
		int pipeFromID = 0;
		int userPipeInfd = -1;
		int userPipeOutfd = -1;

		string file = "";

		vector <pid_t> pidTable;

		cout<<"% ";
		if(!cin.getline(input,CMD_BUFFER))
		{
				exit(1);
		}

		strcpy(in,input);
		strcpy(intact, input);

		if(strlen(input)==0)
		{
				free(input);
				continue;
		}

		if(strcmp(input, "exit") == 0)
		{
		
			char f[100];
			sprintf(f, "*** User '%s' left. ***\n",users[ID].name);
			broadcast(NULL, NULL, f);
			users[ID].has_User = 0;
			
			char rm_f[20];
    		for(int i=0; i<30; i++)
			{
        		if(checkExist(ID+1, i+1))
				{
           			sprintf(rm_f,"user_pipe/%d_%d",ID+1, i+1);
        			remove(rm_f);
        		}
        		if(checkExist(i+1, ID+1))
				{
           			sprintf(rm_f,"user_pipe/%d_%d",i+1, ID+1);
    				remove(rm_f);
    			}
			}
    		shmdt(msg);
    		shmdt(users);
    		exit(0);
		}

		char *str;
		str = strtok(input," "); //用空格切
		int i=0;
		cmds.push_back(vector<string>());
	
		while (str != NULL)
		{
			if(str[0]=='|' || str[0]=='!')
			{
				if(strlen(str) > 1) // number pipe
				{
					if(str[0] == '|')
						PipeType = 0; // normal number pipe
					else
						PipeType = 1; // error number pipe
					str[0]=' ';
					PipeCount=atoi(str);
				}
				else // normal pipe || error normal pipe
				{
					cmds.push_back(vector<string>());
					i++;
				}
				str = strtok (NULL, " ");
				continue;
			}
			else if (str[0] == '>')
			{
				if(strlen(str) == 1)
				{
					FileFlag = 1;
					str = strtok (NULL, " ");
					file = string(str);
					break;
				}
				else
				{
					str[0]=' ';
					pipeToID = atoi(str);
					str = strtok (NULL, " ");
					continue;
				}
			}
			else if(str[0] == '<')
			{
				str[0] = ' ';
				pipeFromID = atoi(str);
				str = strtok (NULL, " ");
				continue;
			}

			// 其他指令等等，因為不影響 pipe or fifo
			// 因此直接放入
			// 上面的特殊符號，就不用放入 cmds 了
			// 直接 strtok 掉
			// 可以發現 i 的變動只在有 normal pipe 出現時
			cmds[i].push_back(string(str));
			str = strtok (NULL, " ");
		}  

		// Check Numpipe
		for(int i=0;i<numPipes.size();i++)
			numPipes[i].count -= 1;

		PipeInfo outnp, innp;
		bool NumPipes_is_In = false;

		if(PipeType > -1) // 0 number pipe, 1 error number pipe, -1 normal pipe
		{
			int f = -1;
			for(int i=0; i<numPipes.size(); i++)
			{
				if(numPipes[i].count == PipeCount) // 相同目的地
				{
					f = i;
					break;
				}
			}

			if(f > -1)
			{
				outnp = numPipes[f];
			}
			else // 沒有相同目的地的 pipe，需要建造
			{
				outnp.count = PipeCount;
				pipe(outnp.fd);
				numPipes.push_back(outnp);
			}
		}

		for(int i=0; i<numPipes.size(); i++)
		{
			if(numPipes[i].count == 0)
			{
				NumPipes_is_In = true;
				innp = numPipes[i];
				numPipes.erase(numPipes.begin()+i);
				close(innp.fd[1]);
				break;
			}
		}

		//User Pipe
		
		if(pipeFromID > 0)
		{
			if(users[pipeFromID-1].has_User == 0)
			{
				printf("*** Error: user #%d does not exist yet. ***\n",pipeFromID);
				free(input);
				continue;
			}

			if(checkExist(pipeFromID,ID+1) == 0)
			{
				printf("*** Error: the pipe #%d->#%d does not exist yet. ***\n", pipeFromID, ID+1);
				free(input);
				continue;
			}
			else
			{
				char s1[20],s2[20];
				sprintf(s1, "user_pipe/%d_%d", pipeFromID, ID+1);
				sprintf(s2, "user_pipe/!%d_%d", pipeFromID, ID+1);
				rename(s1, s2);
				userPipeInfd = open(s2, O_RDONLY);
				char s[100];
				sprintf(s,"*** %s (#%d) just received from %s (#%d) by '%s' ***\n", users[ID].name, ID+1, users[pipeFromID-1].name, pipeFromID, intact);
				broadcast(NULL, NULL, s);
				usleep(10);
			}
		}

		if(pipeToID > 0)
		{
			if(users[pipeToID-1].has_User == 0)
			{
				printf("*** Error: user #%d does not exist yet. ***\n", pipeToID);
				free(input);
				continue;
			}

			if(checkExist(ID+1, pipeToID))
			{
				printf("*** Error: the pipe #%d->#%d already exists. ***\n",ID+1,pipeToID);
				free(input);
				continue;
			}
			else
			{
				char s1[20];
				sprintf(s1,"user_pipe/%d_%d",ID+1,pipeToID);
				userPipeOutfd = open(s1, O_CREAT|O_WRONLY|O_TRUNC, S_IRUSR|S_IWUSR);
				char s[100];
				sprintf(s,"*** %s (#%d) just piped '%s' to %s (#%d) ***\n",users[ID].name, ID+1, intact, users[pipeToID-1].name, pipeToID);
				broadcast(NULL, NULL, s);
			}
		}

		// Executing --

		// build-in cmds
		string tmp = cmds[0][0];
		if(tmp == "who" || tmp == "tell" || tmp == "yell" || tmp == "name" || tmp == "printenv" || tmp == "setenv")
		{

			if(cmds[0][0]=="who")
				who();
			else if(cmds[0][0]=="tell")
			{
				int to = atoi(cmds[0][1].c_str());

				char *s = strchr(in,' '); // %tell ^ 3 Hello World.
				s = strchr(s+1,' '); // %tell ^3 Hello World. -> %tell 3 ^ Hello World.
				s++; // %tell 3 ^Hello World.
				while(*s==' ')s++;
				tell(to,s);
			}
			else if(cmds[0][0]=="yell") // %yell Good morning everyone.
			{
				char *s=strchr(in,' '); // %yell ^ Good morning everyone.
				s++; //// %yell ^Good morning everyone.
				while(*s==' ')s++;
				yell(s);
			}
			else if(cmds[0][0]=="name")
			{
				char *s=strchr(in,' '); //%name ^ Mike
				s++; //%name ^Mike
				while(*s==' ')s++;
				name(s);
			}
			else if(cmds[0][0]=="printenv")
			{
				char *env=getenv(cmds[0][1].c_str());
				if(env!=NULL)
					printf("%s\n",env);
			}
			else if(cmds[0][0]=="setenv")
			{
				if(cmds[0].size()>=3)
					setenv(cmds[0][1].c_str(),cmds[0][2].c_str(),1);
			}
			free(input);
			continue;
		}

		// exec not build-in cmds
		int pipe0[2];
		int prePipeRd = 0;
		
		if(NumPipes_is_In)
		{
				prePipeRd = innp.fd[0];
		}
		if(userPipeInfd != -1)
		{
				prePipeRd = userPipeInfd;
		}

		int filefd;
		if(FileFlag == 1)
		{
				filefd = open(file.c_str(), O_CREAT|O_WRONLY|O_TRUNC,S_IRUSR|S_IWUSR);
		}

		for(int i=0; i<cmds.size(); i++) // ls | cat , | 並不會在 cmds 內部，(cmds.size()-i) == 1 就是最後一個
		{
			if((cmds.size()-i)>1)
			{
				if(pipe(pipe0)<0) printf("Pipe Error\n");
			}

			pid_t childpid;
			while((childpid=fork())<0)
			{
				usleep(1000);
			}
			pidTable.push_back(childpid);

			if(childpid==0) // slave 中的 child
			{
				if(((cmds.size()-i)==1) && FileFlag)
				{
					close(STDOUT_FILENO);
					dup2(filefd,STDOUT_FILENO);
					close(filefd);
				}

				if(prePipeRd>0)
				{     
					close(STDIN_FILENO);           //close stdin
					dup2(prePipeRd,STDIN_FILENO);  //pre read to stdin
					close(prePipeRd);   //close pre
				}

				if((cmds.size()-i)>1) //Not last command
				{     
					close(STDOUT_FILENO);           //close stdout
					dup2(pipe0[1],STDOUT_FILENO);   //write to stdout
					close(pipe0[0]);    //close read
					close(pipe0[1]);    //close write
				}

				if((cmds.size()-i)==1) //Last Command
				{    
					if(PipeType>-1) // number pipe or error number pipe
					{
						close(STDOUT_FILENO);
						dup2(outnp.fd[1],STDOUT_FILENO);
						if(PipeType==1)
						{
							close(STDERR_FILENO);
							dup2(outnp.fd[1],STDERR_FILENO);
						}
						close(outnp.fd[0]);
						close(outnp.fd[1]);
					}
					if(userPipeOutfd!=-1)
					{
						close(STDOUT_FILENO);
						dup2(userPipeOutfd,STDOUT_FILENO);
						close(STDERR_FILENO);
						dup2(userPipeOutfd,STDERR_FILENO);
						close(userPipeOutfd);
					}  
				}

				Exec(cmds[i]);
			}
			else // slave 中的 parent, 不用被更改 fd，快結束時要關掉附加的東東
			{
				if(prePipeRd>0) // parent 用不到 其他 fd
				{        
						close(prePipeRd);   
				}

				if((cmds.size()-i)>1) // 透過 normal pipe 串接了，現在輸出關掉 pipe，下個輸入換成從 pipe 來
				{
						close(pipe0[1]);    //close write
						prePipeRd=pipe0[0]; //read to next process
				}

				if(FileFlag && (cmds.size()-i) == 1)
				{
						close(filefd);
				}

				if((cmds.size()-i)==1&&userPipeOutfd!=-1)
				{
						close(userPipeOutfd);
				}
			}

	 	}

	    if(PipeType == -1) // normal pipe
		{
			for(int i=0;i<pidTable.size();i++)
			{
				pid_t pid=pidTable[i];
				int status;
				waitpid(pid,&status,0);
			}
			if(userPipeInfd!=-1)
			{
				char s[20];
				sprintf(s,"user_pipe/!%d_%d",pipeFromID,ID+1);
				remove(s);
			}
	    }
	    free(input);
	}
    return;
}

void Server_Handler(int signal)
{
	if(signal == SIGCHLD)
	{
		int status;
		while(waitpid(-1, &status, WNOHANG) > 0){}

		for(int i=0; i<30; i++)
		{
			if(users[i].has_User == 1)
			{
				if(kill(users[i].pid, 0) < 0)
				{
					char s[100];
					users[i].has_User = 0;
					sprintf(s,"*** User '%s' left. ***\n", users[ID].name);
					strcpy(msg->str, s);

					for(int j=0; j<30; j++)
						if(users[j].has_User == 1)
								kill(users[j].pid, SIGUSR1);

					char usr_pipe_addr[20];
					for(int j=0; j<30; j++)
					{
						if(checkExist(i+1, j+1) == 1)
						{
							sprintf(usr_pipe_addr, "user_pipe/%d_%d", i+1, j+1);
							remove(usr_pipe_addr);
						}

						if(checkExist(j+1,i+1) == 1)
						{
							sprintf(usr_pipe_addr, "user_pipe/%d_%d", j+1, i+1);
							remove(usr_pipe_addr);
						}
					}
					break;
				}
			}
		}
    }
    else
	{
		shmdt(users);
		shmdt(msg);
		shmctl(shm_1, IPC_RMID, (shmid_ds*)0);
		shmctl(shm_2, IPC_RMID, (shmid_ds*)0);
		exit(1);
    }
}

int main(int argc, char *argv[])
{
	int ser_sockfd, sla_sockfd, status;
	if (argc != 2)
		return 0;
	
	clearenv();

	unsigned short port = (unsigned short)atoi(argv[1]);

	if ((ser_sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
	{
		cerr << "Socket create fail.\n";
		exit(0);
	}
	struct sockaddr_in ser_addr, cli_addr;
	int enable = 1;
	
	memset((char *) &ser_addr, 0, sizeof(ser_addr));
	ser_addr.sin_family = AF_INET;
	ser_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	ser_addr.sin_port = htons(port);

	if (setsockopt(ser_sockfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) < 0) //設定 socket
    {
        cerr << "Error: setsockopt failed" << endl;
        exit(1);
    }
    if (bind(ser_sockfd, (sockaddr *)&ser_addr, sizeof(ser_addr)) < 0) 
    {
        perror("bind");
        cerr << "Error: bind failed" << endl;
        exit(1);
    }
    if (listen(ser_sockfd, 30) < 0) 
    {
        cerr << "Error: listen failed" << endl;
        exit(1);
    }

	shm_1 = shmget(SHM_Users_KEYS, sizeof(User)*30, 0666|IPC_CREAT);
	shm_2 = shmget(SHM_Broadcast_Msg_KEYS, sizeof(BroadcastMsg), 0666|IPC_CREAT);
	users = (User*)shmat(shm_1, (char*)0, 0);
	msg = (BroadcastMsg*)shmat(shm_2, (char*)0, 0);

	signal(SIGCHLD, Server_Handler);
	signal(SIGINT, Server_Handler);

	while(1)
	{
		socklen_t cli_len = sizeof(cli_addr);
		sla_sockfd = accept(ser_sockfd, (sockaddr *)&cli_addr, &cli_len);
		cout<<"hi"<<endl;
		for (ID=0; ID<30; ID++)
		{
			if (users[ID].has_User == 0)
				break;
		}

		pid_t slave_pid = fork();
        while (slave_pid < 0) 
		{
            slave_pid = fork();
        }

		//start fork process
		if(slave_pid == 0)
		{
			close(ser_sockfd);
			close(STDIN_FILENO);
			close(STDOUT_FILENO);
			close(STDERR_FILENO);
			dup2(sla_sockfd, STDIN_FILENO);
			dup2(sla_sockfd, STDOUT_FILENO);
			dup2(sla_sockfd, STDERR_FILENO);
			close(sla_sockfd);

			usleep(10);
			np_shell();
			exit(0);
		}
		else
		{
			users[ID].has_User = 1;
			strcpy(users[ID].name, "(no name)");

			char str[INET_ADDRSTRLEN];
    		inet_ntop(AF_INET, &(cli_addr.sin_addr), str, INET_ADDRSTRLEN);            		
           	int port = ntohs(cli_addr.sin_port);
        	char ip_addr[100];
    		sprintf(ip_addr,"%s:%d", str, port);
       		strcpy(users[ID].address, ip_addr);

            users[ID].pid = slave_pid;

            close(sla_sockfd);
		}
	}
}

