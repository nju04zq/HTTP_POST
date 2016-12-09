### Normal mode
```
gcc -g -o client client.c queue.c util.c task_queue.c -Wall -lpthread
gcc -g -o server server.c queue.c util.c task_queue.c -Wall -lpthread
./server
# in another terminal
./client
```

### Debug mode
```
gcc -g -o client client.c queue.c util.c task_queue.c -Wall -lpthread -D_DEBUG_MODE_
gcc -g -o server server.c queue.c util.c task_queue.c -Wall -lpthread
./server
# in another terminal
./client >& post.log
# analyze client post timecost
python read_client_post_log.py
```
