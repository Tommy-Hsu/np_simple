# NP_project_2

## <demo 題目>

### 1. fifo 與 pipe 的差別

    fifo 可以用於任意兩個 processes 之間傳遞。pipe 只能在有父子關係的 processes 之間傳遞。
  
### 2. server_2 與 server_3 的在環境設定上的差別

    server_2 使用 Single-Process, Concurrent Servers (TCP)，設定環境變數的做法為，將個別 client 之環境變數設定成表格，輪到他們想設定時，即用個別的環境變數表去將整個環境設定。
    
    server_3 使用 Concurrent, Connection-Oriented Servers，因為有多個 slave，每個環境互不影響，可以直接設定。
    
## 手改題目

![316495052_1181986296058519_7020162176766458302_n](https://user-images.githubusercontent.com/65523042/203580608-9dbbd099-20d7-42e6-a2b0-9f54800711f8.jpg)
