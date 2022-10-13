#include <stdio.h>
#include <iostream>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <string>
#include <vector>

using namespace std;

int main(int argc , char *argv[]){
    //NumberPipe* numberpipe = new NumberPipe();
    vector<string> envp;
    envp.push_back("PATH=bin:.");

    while (1){
        cout << "% ";
        string command;
        getline(cin, command);

        if(command.find("exit") != string::npos){
            exit(0);
        }
    }
    return 0;
}