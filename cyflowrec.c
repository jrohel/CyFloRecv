// SPDX-License-Identifier: GPL-2.0-only
// Copyright 2024 Jaroslav Rohel, jaroslav.rohel@gmail.com

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>


static const char CYFLOWREC_VERSION[] = "0.3.0";

static const char ARG_HELP[] = "--help";
static const char ARG_PORT_DEV[] = "--port-dev";
static const char ARG_STORAGE_CREATE_DIRS[] = "--storage-create-dirs";
static const char ARG_STORAGE_DIR[] = "--storage-dir";
static const char ARG_STORAGE_FILE_EXISTS[] = "--storage-file-exists";


enum log_priority { LOG_ERROR, LOG_WARNING, LOG_INFO, LOG_DEBUG };
static const char * const log_priority_strings[] = {"ERROR", "WARNING", "INFO", "DEBUG"};


enum storage_file_exists_policy { FILE_REPLACE, FILE_DROP };

static const char * port_dev = NULL;
static bool storage_create_dirs = false;
static const char * storage_dir = NULL;
static enum storage_file_exists_policy file_exists_policy = FILE_REPLACE;

static char * my_strdup(const char * src) {
    const size_t size = strlen(src) + 1;
    char * const ret = malloc(size);
    if (!ret) {
        perror("strdup: malloc");
        assert(ret != NULL);
    }
    memcpy(ret, src, size);
    return ret;
}


// Performs string formatting like standard `vsprintf`. Unlike it, it stores the result in the newly allocated memory.
// The caller takes ownership of the memory and must free it with `free`.
static char * vsprintf_malloc(const char * restrict format, va_list ap) {
    char * ret = NULL;

    va_list ap_copy;
    va_copy(ap_copy, ap);
    const int required_size = vsnprintf(NULL, 0, format, ap_copy) + 1;
    va_end(ap_copy);

    if (required_size > 0) {
        ret = malloc(required_size);
        if (!ret) {
            perror("sprintf_malloc: malloc");
            assert(ret != NULL);
        }
        vsnprintf(ret, required_size, format, ap);
    }

    return ret;
}

// Performs string formatting like standard `sprintf`. Unlike it, it stores the result in the newly allocated memory.
// The caller takes ownership of the memory and must free it with `free`.
static char * sprintf_malloc(const char * restrict format, ...) {
    va_list ap;
    va_start(ap, format);
    char * const ret = vsprintf_malloc(format, ap);
    va_end(ap);
    return ret;
}


static void log_msg(enum log_priority priority, const char * restrict message) {
    // Create ISO 8601 timestamp
    const time_t t = time(NULL);
    struct tm * tm = gmtime(&t);
    if (tm == NULL) {
        perror("gmtime");
        return;
    }
    char time_buf[64];
    strftime(time_buf, sizeof(time_buf), "%FT%TZ", tm);

    char * const line = sprintf_malloc("%s %s %s", time_buf, log_priority_strings[priority], message);
    puts(line);
    free(line);
}


static void log_fmtmsg(enum log_priority priority, const char * restrict message_format, ...) {
    va_list ap;
    va_start(ap, message_format);
    char * const message = vsprintf_malloc(message_format, ap);
    va_end(ap);
    log_msg(priority, message);
    free(message);
}


// Creates every missing directory in path.
// If the path does not end with '/', the last element is treated as a filename and is not created.
static bool mkdirs(const char * path) {
    char * const path_tmp = my_strdup(path);
    char * sep = strchr(path_tmp + 1, '/');
    while (sep) {
        *sep = '\0';
        if (mkdir(path_tmp, 0755) == -1 && errno != EEXIST) {
            log_fmtmsg(LOG_ERROR, "Cannot create directory \"%s\": %s", path_tmp, strerror(errno));
            free(path_tmp);
            return false;
        }
        *sep = '/';
        sep = strchr(sep + 1, '/');
    }
    free(path_tmp);
    return true;
}


// 9600 baud, 8 bits, 2 stop bits and no parity check
static bool set_port(int fd) {
    struct termios tty;

    if (tcgetattr(fd, &tty) != 0) {
        log_fmtmsg(LOG_ERROR, "Cannot get port attributes: %s", strerror(errno));
        return false;
    }

    cfsetospeed(&tty, B9600);
    cfsetispeed(&tty, B9600);

    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;  // 8-bit chars
    tty.c_cflag &= ~(PARENB | PARODD);           // shut off parity
    tty.c_cflag |= CSTOPB;                       // two stop bits
    tty.c_cflag |= CLOCAL | CREAD;               // ignore modem controls, enable receiver
    //    tty.c_cflag &= ~CRTSCTS;                     // Disable RTS/CTS hardware flow control

    tty.c_lflag = 0;  // no signaling chars, no echo,

    tty.c_iflag &= ~(IXON | IXOFF | IXANY);  // turn off xon/xoff flow ctrl
    tty.c_iflag &=
        ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);  // Disable any special handling of received bytes


    // no canonical processing
    tty.c_oflag = 0;  // no remapping, no delays

    tty.c_cc[VMIN] = 128;  // wait for 128 characters
    tty.c_cc[VTIME] = 5;   // intercharacter timeout 5 deciseconds (0.5 second)

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        log_fmtmsg(LOG_ERROR, "Cannot set port attributes: %s", strerror(errno));
        return false;
    }

    return true;
}


static ssize_t read_timeout(int fd, void * buf, size_t nbytes, int timeout) {
    struct pollfd fds = {.fd = fd, .events = POLLIN, .revents = 0};
    const int poll_ret = poll(&fds, 1, timeout);
    if (poll_ret == -1) {
        log_fmtmsg(LOG_ERROR, "poll: %s", strerror(errno));
        return -1;
    }
    if ((fds.revents & POLLERR) != 0) {
        log_msg(LOG_ERROR, "Cannot read serial device");
        return -1;
    }
    if ((fds.revents & POLLIN) == 0) {
        return 0;
    }
    return read(fd, buf, nbytes);
}


// Transfered data contain [KEY]<value> pairs folowed by file content. [FILENAME] and [FILESIZE] are mandatory.
// Example: [FILENAME]<A0000001.FCS>[FILESIZE]<9732>FCS2.0...
static void recv_loop() {
    int port_fd;
    if ((port_fd = open(port_dev, O_RDWR | O_NOCTTY)) == -1) {
        log_fmtmsg(LOG_ERROR, "Cannot open port \"%s\": %s", port_dev, strerror(errno));
        return;
    }
    if (!set_port(port_fd)) {
        close(port_fd);
        return;
    }

    int file_fd = -1;

    char * rcv_file_name = NULL;
    size_t rcv_file_size = 0;
    char * storage_file_path = NULL;
    char buf[128];
    size_t buf_data_len = 0;

    size_t requested_reading_len = 1;
    int timeout_ms = -1;  // timeout in miliseconds, -1 = infinite
    size_t total_rcv_file_bytes;
    enum read_state {
        READ_START,
        READ_NEXT,
        READ_KEY,
        READ_FILE_NAME,
        READ_FILE_SIZE,
        READ_UNKNOWN_VALUE,
        READ_FILE,
        READ_DISCARD_UNTIL_TIMEOUT
    } state = READ_START;
    bool discard_message_logged;
    while (true) {
        const ssize_t read_len = read_timeout(port_fd, buf + buf_data_len, requested_reading_len, timeout_ms);

        if (read_len == -1) {
            break;
        }

        if (read_len == 0) {
            if (state != READ_START && state != READ_DISCARD_UNTIL_TIMEOUT) {
                log_msg(LOG_ERROR, "Timeout, data reception not completed");
            }
            if (file_fd != -1) {
                close(file_fd);
                file_fd = -1;
            }
            if (storage_file_path) {
                free(storage_file_path);
                storage_file_path = NULL;
            }
            if (rcv_file_name) {
                free(rcv_file_name);
                rcv_file_name = NULL;
            }
            rcv_file_size = 0;
            buf_data_len = 0;
            requested_reading_len = 1;
            timeout_ms = -1;
            if (state == READ_DISCARD_UNTIL_TIMEOUT) {
                log_msg(LOG_INFO, "Discarding of received data stopped. Ready to receive the next file");
            }
            state = READ_START;
            continue;
        }

        switch (state) {
            case READ_START:
                if (buf[0] != '[') {
                    log_msg(LOG_ERROR, "Unexpected character received");
                    continue;
                }
                discard_message_logged = false;
                log_msg(LOG_INFO, "Start receiving");
                timeout_ms = 1000;
                state = READ_KEY;
                break;
            case READ_KEY:
                if (buf[buf_data_len] == ']') {
                    buf[buf_data_len] = '\0';
                    if (strcmp(buf, "FILENAME") == 0) {
                        if (rcv_file_name) {
                            log_msg(LOG_WARNING, "Received FILENAME key again");
                            free(rcv_file_name);
                            rcv_file_name = NULL;
                            rcv_file_size = 0;
                        }
                        state = READ_FILE_NAME;
                    } else if (strcmp(buf, "FILESIZE") == 0) {
                        if (!rcv_file_name) {
                            log_msg(LOG_ERROR, "Received FILESIZE key before FILENAME");
                            state = READ_DISCARD_UNTIL_TIMEOUT;
                            buf_data_len = 0;
                            requested_reading_len = sizeof(buf);
                            break;
                        }
                        if (rcv_file_size > 0) {
                            log_msg(LOG_ERROR, "Received FILESIZE key again");
                            state = READ_DISCARD_UNTIL_TIMEOUT;
                            buf_data_len = 0;
                            requested_reading_len = sizeof(buf);
                            break;
                        }
                        state = READ_FILE_SIZE;
                    } else {
                        log_fmtmsg(LOG_DEBUG, "Received unknown key: %s", buf);
                        state = READ_UNKNOWN_VALUE;
                    }
                    buf_data_len = 0;
                } else {
                    if (++buf_data_len >= sizeof(buf)) {
                        log_msg(LOG_ERROR, "Received key name is too long");
                        state = READ_DISCARD_UNTIL_TIMEOUT;
                        buf_data_len = 0;
                        requested_reading_len = sizeof(buf);
                    }
                }
                break;
            case READ_FILE_NAME:
                if (buf[buf_data_len] == '>') {
                    buf[buf_data_len] = '\0';
                    bool bad_filename = false;
                    for (const char * ch = buf + 1; *ch != '\0'; ++ch) {
                        if (*ch != '.' && !isdigit(*ch) && !isupper(*ch) && !islower(*ch)) {
                            log_fmtmsg(LOG_ERROR, "Received FILENAME contains forbidden characters: %s", buf + 1);
                            bad_filename = true;
                            state = READ_DISCARD_UNTIL_TIMEOUT;
                            requested_reading_len = sizeof(buf);
                            break;
                        }
                    }
                    if (!bad_filename) {
                        log_fmtmsg(LOG_DEBUG, "Received FILENAME: %s", buf + 1);
                        rcv_file_name = my_strdup(buf + 1);
                        state = READ_NEXT;
                    }
                    buf_data_len = 0;
                } else {
                    if (++buf_data_len >= sizeof(buf)) {
                        log_msg(LOG_ERROR, "Received FILENAME is too long");
                        state = READ_DISCARD_UNTIL_TIMEOUT;
                        buf_data_len = 0;
                        requested_reading_len = sizeof(buf);
                    }
                }
                break;
            case READ_FILE_SIZE:
                if (buf[buf_data_len] == '>') {
                    buf[buf_data_len] = '\0';
                    char * endptr;
                    long file_size = strtol(buf + 1, &endptr, 10);
                    if (file_size < 0 || endptr != buf + buf_data_len) {
                        log_fmtmsg(LOG_ERROR, "Received invalid FILESIZE: %s", buf + 1);
                        state = READ_DISCARD_UNTIL_TIMEOUT;
                        buf_data_len = 0;
                        requested_reading_len = sizeof(buf);
                        break;
                    }
                    log_fmtmsg(LOG_DEBUG, "Received FILESIZE: %s", buf + 1);
                    rcv_file_size = file_size;
                    state = READ_NEXT;
                    buf_data_len = 0;
                } else {
                    if (++buf_data_len >= sizeof(buf)) {
                        log_msg(LOG_ERROR, "Received FILESIZE is too long");
                        state = READ_DISCARD_UNTIL_TIMEOUT;
                        buf_data_len = 0;
                        requested_reading_len = sizeof(buf);
                    }
                }
                break;
            case READ_UNKNOWN_VALUE:
                if (buf[buf_data_len] == '>') {
                    state = READ_NEXT;
                }
                break;
            case READ_NEXT:
                if (buf[0] == '[') {
                    state = READ_KEY;
                } else {
                    if (rcv_file_size == 0) {
                        log_msg(LOG_ERROR, "Missing FILESIZE");
                        state = READ_DISCARD_UNTIL_TIMEOUT;
                        buf_data_len = 0;
                        requested_reading_len = sizeof(buf);
                        break;
                    }
                    storage_file_path = sprintf_malloc("%s/%s", storage_dir, rcv_file_name);
                    log_fmtmsg(
                        LOG_INFO,
                        "Incoming file \"%s\" with length %u will be stored in \"%s\"",
                        rcv_file_name,
                        (unsigned int)rcv_file_size,
                        storage_file_path);
                    if (storage_create_dirs) {
                        mkdirs(storage_file_path);
                    }
                    file_fd =
                        open(storage_file_path, O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
                    if (file_fd == -1) {
                        const int origin_errno = errno;
                        if (origin_errno == EEXIST) {
                            if (file_exists_policy == FILE_REPLACE) {
                                log_fmtmsg(
                                    LOG_WARNING,
                                    "The file \"%s\" already exists in the storage and will be replaced by "
                                    "the received file \"%s\"",
                                    storage_file_path,
                                    rcv_file_name);
                                file_fd = open(
                                    storage_file_path,
                                    O_WRONLY | O_CREAT | O_TRUNC,
                                    S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
                            } else {
                                log_fmtmsg(
                                    LOG_WARNING,
                                    "The file \"%s\" already exists in the storage, the received file \"%s\" "
                                    "will be dropped",
                                    storage_file_path,
                                    rcv_file_name);
                            }
                        }
                        if (file_fd == -1 && (origin_errno != EEXIST || file_exists_policy == FILE_REPLACE)) {
                            log_fmtmsg(
                                LOG_ERROR,
                                "Cannot open/create file \"%s\", received file \"%s\" will no be stored: %s",
                                storage_file_path,
                                rcv_file_name,
                                strerror(errno));
                        }
                    }
                    state = READ_FILE;
                    buf_data_len = 1;
                    total_rcv_file_bytes = 0;
                    const size_t bytes_to_end = rcv_file_size - 1;  // one byte is received in buf
                    requested_reading_len = bytes_to_end > sizeof(buf) - 1 ? sizeof(buf) - 1 : bytes_to_end;
                }
                break;
            case READ_FILE:
                buf_data_len += read_len;
                total_rcv_file_bytes += buf_data_len;

                if (file_fd != -1) {
                    size_t written = 0;
                    do {
                        const ssize_t write_ret = write(file_fd, buf + written, buf_data_len - written);
                        if (write_ret == -1) {
                            log_fmtmsg(
                                LOG_ERROR,
                                "Cannot write to file \"%s\", received file \"%s\" will be truncated: %s",
                                storage_file_path,
                                rcv_file_name,
                                strerror(errno));
                            close(file_fd);
                            file_fd = -1;
                            break;
                        }
                        written += write_ret;
                    } while (written < buf_data_len);
                }

                buf_data_len = 0;
                if (total_rcv_file_bytes >= rcv_file_size) {
                    if (file_fd != -1) {
                        close(file_fd);
                        file_fd = -1;
                        log_fmtmsg(
                            LOG_INFO,
                            "The file \"%s\" was received and saved as \"%s\"",
                            rcv_file_name,
                            storage_file_path);
                    } else {
                        log_fmtmsg(LOG_INFO, "The file \"%s\" was received but discarded or truncated", rcv_file_name);
                    }
                    free(storage_file_path);
                    storage_file_path = NULL;
                    free(rcv_file_name);
                    rcv_file_name = NULL;
                    rcv_file_size = 0;
                    requested_reading_len = 1;
                    timeout_ms = -1;
                    state = READ_START;
                    break;
                }
                const size_t bytes_to_end = rcv_file_size - total_rcv_file_bytes;
                requested_reading_len = bytes_to_end > sizeof(buf) ? sizeof(buf) : bytes_to_end;
                break;
            case READ_DISCARD_UNTIL_TIMEOUT:
                if (!discard_message_logged) {
                    log_msg(LOG_WARNING, "Start discarding received data until the no-data timeout expires");
                    discard_message_logged = true;
                }
                break;
        }
    }

    if (file_fd != -1) {
        close(file_fd);
    }
    if (storage_file_path) {
        free(storage_file_path);
    }
    if (rcv_file_name) {
        free(rcv_file_name);
    }
    close(port_fd);
}


static void print_help() {
    const int LEFT_COLUMN_WIDTH = 33;

    printf("CyFlowRec %s, Copyright 2024 Jaroslav Rohel <jaroslav.rohel@gmail.com>\n", CYFLOWREC_VERSION);
    printf(
        "CyFlowRec comes with ABSOLUTELY NO WARRANTY. This is free software\n"
        "and you are welcome to redistribute it under the terms of the GNU GPL v2.\n\n");

    printf(
        "Usage: cyflowrec [%s] %s=<port> [%s=<policy>] %s=<path> [%s=<0/1>]\n\n",
        ARG_HELP,
        ARG_PORT_DEV,
        ARG_STORAGE_FILE_EXISTS,
        ARG_STORAGE_DIR,
        ARG_STORAGE_CREATE_DIRS);

    printf("%s%*sprint this help\n", ARG_HELP, (int)(LEFT_COLUMN_WIDTH - sizeof(ARG_HELP)), "");
    printf(
        "%s=<path>%*spath to the serial port device (eg /dev/ttyS0)\n",
        ARG_PORT_DEV,
        (int)(LEFT_COLUMN_WIDTH - sizeof(ARG_PORT_DEV) - 7),
        "");
    printf(
        "%s=<0/1>%*sdisable/enable the creation of missing\n"
        "%*sdirectories in the storage path\n"
        "%*s(0 - disable, 1 - enable; disabled by default)\n",
        ARG_STORAGE_CREATE_DIRS,
        (int)(LEFT_COLUMN_WIDTH - sizeof(ARG_STORAGE_CREATE_DIRS) - 6),
        "",
        LEFT_COLUMN_WIDTH,
        "",
        LEFT_COLUMN_WIDTH,
        "");
    printf(
        "%s=<path>%*spath to the storage directory\n",
        ARG_STORAGE_DIR,
        (int)(LEFT_COLUMN_WIDTH - sizeof(ARG_STORAGE_DIR) - 7),
        "");
    printf(
        "%s=<policy>%*swhat to do if a file with the given name\n"
        "%*salready exists in the storage\n"
        "%*s(drop - drop the received file, replace -\n"
        "%*sreplace the file in storage with\n"
        "%*sthe received one; replace by default)\n",
        ARG_STORAGE_FILE_EXISTS,
        (int)(LEFT_COLUMN_WIDTH - sizeof(ARG_STORAGE_FILE_EXISTS) - 9),
        "",
        LEFT_COLUMN_WIDTH,
        "",
        LEFT_COLUMN_WIDTH,
        "",
        LEFT_COLUMN_WIDTH,
        "",
        LEFT_COLUMN_WIDTH,
        "");
}


// If the argument `arg_name` is found at position `idx` its value is stored into `value`
// and the function returns true. If an error occurs, the function returns false.
// If `idx` points after the arguments, or there is another argument at that position,
// the function does nothing and returns true.
static bool arg_parse_value(int argc, char * argv[], int * idx, const char * arg_name, const char ** value) {
    if (*idx >= argc) {
        return true;
    }
    const char * const arg = argv[*idx];
    const size_t arg_len = strlen(arg_name);
    if (strncmp(arg, arg_name, arg_len) == 0) {
        switch (arg[arg_len]) {
            case '\0':
                if (++*idx == argc) {
                    fprintf(stderr, "Missing value for argument %s\n", arg);
                    return false;
                }
                *value = arg;
                break;
            case '=':
                *value = arg + arg_len + 1;
                break;
            default:
                return true;
        }
    } else {
        return true;
    }
    ++*idx;
    return true;
}


int main(int argc, char * argv[]) {
    bool args_error = false;
    const char * create_dirs = NULL;
    const char * file_exists = NULL;

    for (int i = 1; i < argc;) {
        const int parsed_idx = i;
        if (!arg_parse_value(argc, argv, &i, ARG_PORT_DEV, &port_dev)) {
            return 1;
        }
        if (!arg_parse_value(argc, argv, &i, ARG_STORAGE_DIR, &storage_dir)) {
            return 1;
        }
        if (!arg_parse_value(argc, argv, &i, ARG_STORAGE_CREATE_DIRS, &create_dirs)) {
            return 1;
        }
        if (!arg_parse_value(argc, argv, &i, ARG_STORAGE_FILE_EXISTS, &file_exists)) {
            return 1;
        }
        if (i < argc && strcmp(argv[i], ARG_HELP) == 0) {
            print_help();
            return 0;
        }
        if (i == parsed_idx) {
            fprintf(stderr, "Unknown argument %s\n", argv[i]);
            args_error = true;
            break;
        }
    }

    if (!storage_dir) {
        fprintf(stderr, "Missing %s=<path> argument\n", ARG_STORAGE_DIR);
        args_error = true;
    }
    if (!port_dev) {
        fprintf(stderr, "Missing %s=<port> argument\n", ARG_PORT_DEV);
        args_error = true;
    }

    if (create_dirs) {
        if (strcmp(create_dirs, "1") == 0) {
            storage_create_dirs = true;
        } else if (strcmp(create_dirs, "0") != 0) {
            fprintf(stderr, "Bad value for argument %s: %s\n", ARG_STORAGE_CREATE_DIRS, create_dirs);
            args_error = true;
        }
    }

    if (file_exists) {
        if (strcmp(file_exists, "drop") == 0) {
            file_exists_policy = FILE_DROP;
        } else if (strcmp(file_exists, "replace") != 0) {
            fprintf(stderr, "Bad value for argument %s: %s\n", ARG_STORAGE_FILE_EXISTS, file_exists);
            args_error = true;
        }
    }

    if (args_error) {
        fprintf(stderr, "Add \"--help\" for more information about the arguments.\n");
        return 1;
    }

    recv_loop();

    return 1;
}
