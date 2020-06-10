#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <uv.h>
#include <string.h>
#include "commands.h"
#include "transport.h"
#include "terminal.h"

uv_loop_t *loop;
uv_pipe_t server_pipe;
uv_tty_t tty_stdout;
uv_work_t work;
typedef struct {
    uv_write_t req;
    uv_buf_t buf;
} write_req_t;

typedef struct {
    uint8_t type;
    char *cmd;
} xdag_cmd_t;

typedef struct {
    size_t recved_length;
    size_t recv_size;
    char *recv_buf;
} xdag_session_t;

static void command_work(uv_work_t* work);
static void command_complete(uv_work_t* work, int status);
static void on_stdout_write(uv_write_t* req, int status);
static void exec_xdag_command(uv_handle_t* handle, char *cmd);

static void free_write_req(uv_write_t *req) {
    write_req_t *wr = (write_req_t*) req;
    free(wr->buf.base);
    free(wr);
}

static void alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
    buf->base = (char*)malloc(suggested_size);
    buf->len = suggested_size;
}

// start pipe server
static void on_server_write(uv_write_t *req, int status) {
    if (status < 0) {
        fprintf(stderr, "pipe write error %s\n", uv_err_name(status));
    }
    free_write_req(req);
}

static void exec_xdag_command(uv_handle_t* handle, char *cmd) {
    FILE *tmp_fp = tmpfile();
    int offset = 0;
    if(xdag_command(cmd, tmp_fp) < 0) {
        uv_close((uv_handle_t*) handle, NULL);
    }
    fseek(tmp_fp, 0, SEEK_END);
    size_t len = ftell(tmp_fp);
    fseek(tmp_fp, 0, SEEK_SET);
    if(uv_handle_get_type(handle) == UV_NAMED_PIPE) {
        offset = sizeof(size_t);
    }
    if(len > 0) {
        write_req_t *req = (write_req_t*) malloc(sizeof(write_req_t));
        len += offset;
        req->buf = uv_buf_init((char*) malloc(len) , len);
        if(offset) {
            memcpy(req->buf.base, &len, offset);
        }
        fread(req->buf.base + offset, len - offset, 1, tmp_fp);
        uv_write((uv_write_t*) req, (uv_stream_t*)handle, &req->buf, 1, on_server_write);
    }
    fclose(tmp_fp);
}

static void on_server_read(uv_stream_t *client, ssize_t nread, const uv_buf_t *buf) {
    if (nread > 0) {
        exec_xdag_command((uv_handle_t*)client, buf->base);
    }

    if (nread < 0) {
        if (nread != UV_EOF) {
            fprintf(stderr, "pipe read error %s\n", uv_err_name(nread));
        }
        uv_close((uv_handle_t*) client, NULL);
    }
}

static void remove_sock(int sig) {
    uv_fs_t req;
    uv_fs_unlink(loop, &req, UNIX_SOCK, NULL);
    exit(0);
}

static void on_server_new_connection(uv_stream_t *server, int status) {
    if (status == -1) {
        return;
    }
    uv_pipe_t *client = (uv_pipe_t*) malloc(sizeof(uv_pipe_t));
    uv_pipe_init(loop, client, 0);
    if (uv_accept(server, (uv_stream_t*) client) == 0) {
        uv_read_start((uv_stream_t*) client, alloc_buffer, on_server_read);
    } else {
        uv_close((uv_handle_t*) client, NULL);
    }
}

void terminal_server(void *arg)
{
    xdag_init_commands();
    loop = uv_default_loop();
    uv_pipe_init(loop, &server_pipe, 0);
    uv_tty_init(loop, &tty_stdout,1,0);
    signal(SIGINT, remove_sock);
    unlink(UNIX_SOCK);
    int r;
    if ((r = uv_pipe_bind(&server_pipe, UNIX_SOCK))){
        fprintf(stderr, "pipe bind error %s\n", uv_err_name(r));
        exit(-1);
    }
    xdag_cmd_t *xdag_cmd = malloc(sizeof(xdag_cmd_t));
    memset(xdag_cmd, 0, sizeof(xdag_cmd_t));
    xdag_cmd->type = 0;
    work.data = xdag_cmd;
    uv_queue_work(loop, &work, command_work, command_complete);
    if ((r = uv_listen((uv_stream_t*) &server_pipe, 128, on_server_new_connection))){
        fprintf(stderr, "pipe listen error %s\n", uv_err_name(r));
        exit(-1);
    }

    uv_run(loop, UV_RUN_DEFAULT);
}
// end pipe server

// start pipe client
static void on_client_write_pipe(uv_write_t* req, int status);

static void command_work(uv_work_t* work) {
    char *lasts = NULL;
    char *ptr = NULL;
    char cmd[XDAG_COMMAND_MAX] = {0};
    char cmd2[XDAG_COMMAND_MAX] = {0};
    while(ptr == NULL) {
        read_command(cmd);
        strncpy(cmd2, cmd, strlen(cmd));
        ptr = strtok_r(cmd2, " \t\r\n", &lasts);
        if(ptr) break;
    }
    if(!strcmp(ptr, "exit") || !strcmp(ptr, "terminate")) {
        uv_stop(loop);
    }
    if(!strcmp(ptr, "xfer")) {
        uint32_t pwd[4];
        xdag_user_crypt_action(pwd, 0, 4, 4);
        sprintf(cmd2, "pwd=%08x%08x%08x%08x ", pwd[0], pwd[1], pwd[2], pwd[3]);
        strncpy(cmd2 + strlen(cmd2), cmd, strlen(cmd));
        strncpy(cmd, cmd2, strlen(cmd2));
    }

    xdag_cmd_t * xdag_cmd = work->data;
    xdag_cmd->cmd = strdup(cmd);

    if(xdag_cmd->type == 0) {
        exec_xdag_command((uv_handle_t*)&tty_stdout, cmd);
    }
}

static void on_client_read_pipe(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf){
    xdag_session_t  *xd_session = (xdag_session_t*)stream->data;
    if (nread < 0) {
        if (nread != UV_EOF) {
            fprintf(stderr, "pipe read error %s\n", uv_err_name(nread));
        }
        uv_close((uv_handle_t*) stream, NULL);
    } else if(nread > 0) {
        if(xd_session->recv_size == 0) {
            memcpy(&(xd_session->recv_size), buf->base, sizeof(size_t));
            xd_session->recv_buf = malloc(xd_session->recv_size);
            memset(xd_session->recv_buf, 0, xd_session->recv_size);
            xd_session->recved_length = 0;
        }
        memcpy(xd_session->recv_buf + xd_session->recved_length, buf->base, nread);
        xd_session->recved_length += nread;
        if(xd_session->recved_length == xd_session->recv_size) {
            write_req_t *wri = (write_req_t *)malloc(sizeof(write_req_t));
            size_t readable_size = xd_session->recv_size - sizeof(size_t);
            wri->buf = uv_buf_init((char*)malloc(readable_size), readable_size);
            memcpy(wri->buf.base, xd_session->recv_buf + sizeof(size_t), readable_size);
            uv_write((uv_write_t*)wri,(uv_stream_t*)&tty_stdout, &wri->buf,1, on_stdout_write);
            free(xd_session->recv_buf);
            memset(xd_session, 0, sizeof(xdag_session_t));
        }
    }
}

static void command_complete(uv_work_t* work, int status) {
    write_req_t *wri = (write_req_t *)malloc(sizeof(write_req_t));
    xdag_cmd_t *xdag_cmd = work->data;
    if(!xdag_cmd) return;
    char *cmd = xdag_cmd->cmd;
    if(!cmd) return;
    wri->buf = uv_buf_init((char*) malloc(strlen(cmd) + 1), strlen(cmd) + 1);
    memcpy(wri->buf.base, cmd, strlen(cmd) + 1);

    if(xdag_cmd->type == 0) {
        memset(wri->buf.base, 0, strlen(cmd));
        uv_write((uv_write_t*)wri, (uv_stream_t*)&tty_stdout, &wri->buf, 1, on_stdout_write);
    } else if(xdag_cmd->type == 1) {
        uv_write((uv_write_t*)wri, (uv_stream_t*)&server_pipe, &wri->buf, 1, on_client_write_pipe);
        uv_read_start((uv_stream_t*)&server_pipe, alloc_buffer, on_client_read_pipe);
    }
}

static void on_stdout_write(uv_write_t* req, int status) {
    if(status){
        fprintf(stderr, "tty write error %s\n", uv_strerror(status));
        exit(0);
    }
    free_write_req(req);
    uv_queue_work(loop, &work, command_work, command_complete);
}

static void on_client_write_pipe(uv_write_t* req, int status){
    if(status){
        fprintf(stderr, "pipe write error %s\n", uv_strerror(status));
        exit(0);
    }
    free_write_req(req);
}

static void on_client_connect(uv_connect_t* req, int status) {
    if(status < 0){
        fprintf(stderr, "pipe new conect error...\n");
    }
    xdag_session_t *xd_session = malloc(sizeof(xdag_session_t));
    memset(xd_session, 0, sizeof(xdag_session_t));
    uv_handle_set_data((uv_handle_t*)&server_pipe, xd_session);
    uv_read_start((uv_stream_t*)&server_pipe, alloc_buffer, on_client_read_pipe);
}

void terminal_client(void *arg) {
    xdag_init_commands();
    loop = uv_default_loop();
    uv_pipe_init(loop, &server_pipe, 0);
    uv_tty_init(loop, &tty_stdout,1,0);

    xdag_cmd_t *xdag_cmd = malloc(sizeof(xdag_cmd_t));
    memset(xdag_cmd, 0, sizeof(xdag_cmd_t));
    xdag_cmd->type = 1;
    work.data = xdag_cmd;
    uv_queue_work(loop, &work, command_work, command_complete);
    uv_connect_t conn;
    uv_pipe_connect((uv_connect_t*)&conn, &server_pipe, UNIX_SOCK, on_client_connect);
    uv_run(loop, UV_RUN_DEFAULT);
}
