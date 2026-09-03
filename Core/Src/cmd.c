/*
 * cmd.c
 *
 *  Created on: 2026年8月5日
 *      Author: scj
 */

#include "cmd.h"
#include "storage.h"     /* storage_req_t、storageQueueHandle */
#include "log_uart.h"    /* log_printf */
#include <string.h>      /* strtok / strcmp / strlen / memcpy */
#include <stdlib.h>      /* strtoul */

void parse_and_dispatch(char *line)
{
    storage_req_t req = {0};
    char *cmd = strtok(line, " ");
    if (cmd == NULL) return;

    if (strcmp(cmd, "help") == 0) {
        log_printf("  id\r\n  erase <addr>\r\n  write <addr> <text>\r\n  read <addr> <len>\r\n");
        return;
    }
    else if (strcmp(cmd, "id") == 0) {
        req.type = REQ_ID;
    }
    else if (strcmp(cmd, "erase") == 0) {
        char *a = strtok(NULL, " ");
        req.type = REQ_ERASE;
        req.addr = a ? strtoul(a, NULL, 0) : 0;
    }
    else if (strcmp(cmd, "write") == 0) {
        char *a = strtok(NULL, " ");
        char *t = strtok(NULL, "");          /* 剩下整段當資料 */
        if (!a || !t) { log_printf("usage: write <addr> <text>\r\n"); return; }
        req.type = REQ_WRITE;
        req.addr = strtoul(a, NULL, 0);
        req.len  = strlen(t);
        if (req.len > sizeof(req.data)) req.len = sizeof(req.data);
        memcpy(req.data, t, req.len);
    }
    else if (strcmp(cmd, "read") == 0) {
        char *a = strtok(NULL, " ");
        char *l = strtok(NULL, " ");
        req.type = REQ_READ;
        req.addr = a ? strtoul(a, NULL, 0) : 0;
        req.len  = l ? strtoul(l, NULL, 0) : 16;
        if (req.len > sizeof(req.data)) req.len = sizeof(req.data);
    }
    else if (strcmp(cmd, "log") == 0) {
            char *sub = strtok(NULL, " ");
            if (sub == NULL) { log_printf("usage: log write|read|dump|stats|format\r\n"); return; }

            if (strcmp(sub, "write") == 0) {
                char *t = strtok(NULL, "");
                if (!t) { log_printf("usage: log write <text>\r\n"); return; }
                req.type = REQ_LOG_WRITE;
                req.len  = strlen(t);
                if (req.len > sizeof(req.data)) req.len = sizeof(req.data);
                memcpy(req.data, t, req.len);
            }
            else if (strcmp(sub, "read") == 0) {
                char *i = strtok(NULL, " ");
                if (!i) { log_printf("usage: log read <id>\r\n"); return; }
                req.type = REQ_LOG_READ;
                req.addr = strtoul(i, NULL, 0);      /* 借 addr 欄位放 rec_id */
            }
            else if (strcmp(sub, "dump") == 0)   req.type = REQ_LOG_DUMP;
            else if (strcmp(sub, "stats") == 0)  req.type = REQ_LOG_STATS;
            else if (strcmp(sub, "format") == 0) req.type = REQ_LOG_FORMAT;
            else if (strcmp(sub, "corrupt") == 0) req.type = REQ_LOG_CORRUPT;
            else if (strcmp(sub, "partial") == 0) {
                char *t = strtok(NULL, "");
                if (!t) { log_printf("usage: log partial <text>\r\n"); return; }
                req.type = REQ_LOG_PARTIAL;
                req.len  = strlen(t);
                if (req.len > sizeof(req.data)) req.len = sizeof(req.data);
                memcpy(req.data, t, req.len);
            }
            else if (strcmp(sub, "remount") == 0) req.type = REQ_LOG_REMOUNT;
            else { log_printf("unknown: log %s\r\n", sub); return; }
    }

    else {
        log_printf("unknown: %s (try 'help')\r\n", cmd);
        return;
    }

    osMessageQueuePut(storageQueueHandle, &req, 0, osWaitForever);
}
