#pragma once

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "hal/globals.h"
#include "hal/macros.h"
#include "jpeg.h"

void http_post_start();
void http_post_stop();
