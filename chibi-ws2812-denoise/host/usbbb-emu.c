#define _DEFAULT_SOURCE
#include <SDL.h>
#include <SDL_opengl.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include "usbbb.h"

#define LED_COUNT 320
#define SENSOR_ROWS 8
#define SENSOR_COLS 12

// you can override this using the environment variable BBEMU.
// set it to the number of milliseconds per frame.
#define TIME_PER_FRAME 12

struct bb_ctx_s {
  SDL_Thread *event_thread;
  SDL_Window *window;
  SDL_Renderer *renderer;

  uint8_t fb[LED_COUNT*3];
  uint8_t xmit_fb[LED_COUNT*3];
  bool xmit;
  uint16_t sensors[SENSOR_COLS*SENSOR_ROWS];

  bool running;
  bool transmitting;
  int measure_row;
  int xmit_offs;
  int sensor_last_row;
  int ms_per_frame;
  SDL_mutex *mutex_transmitting;
  SDL_mutex *mutex_wait_sensor;
  SDL_cond *cond_wait_sensor;

  uint8_t rbuf[1+SENSOR_ROWS*sizeof(uint16_t)];

  int16_t pos_led[40][40];
  int16_t pos_leds10[10][10][4];
};

BB_API
void bb_get_sensordata(bb_ctx *C, uint16_t sensordata[]) {
  SDL_LockMutex(C->mutex_wait_sensor);
  memcpy(sensordata, C->sensors, sizeof(uint16_t)*SENSOR_COLS*SENSOR_ROWS);
  SDL_UnlockMutex(C->mutex_wait_sensor);
}

static int bb_event_thread(void *d) {
  SDL_Event event;
  bb_ctx *C = (bb_ctx*)d;

	if(NULL == (C->window = SDL_CreateWindow(
          "BB Emulator",
          SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
          640, 640,
          SDL_WINDOW_SHOWN))) {
		return -1;
	}
  if(NULL == (C->renderer = SDL_CreateRenderer(C->window, -1, SDL_RENDERER_ACCELERATED))) {
    return -1;
  }

  C->running = true;
  int next = SDL_GetTicks() + C->ms_per_frame;
  while(C->running) {
    int now = SDL_GetTicks();
    int wait = (next - now);
    if(wait < 0) {
      next += C->ms_per_frame;
      continue;
    }
    if(!SDL_WaitEventTimeout(&event, wait)) {
      next += C->ms_per_frame;
      SDL_LockMutex(C->mutex_transmitting);
      if(C->xmit) {
        C->xmit = false;
        SDL_SetRenderDrawBlendMode(C->renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(C->renderer, 0, 0, 0, 255);
        SDL_RenderClear(C->renderer);
        SDL_Rect r;
        r.w = 28;
        r.h = 56;
        uint8_t *c = C->xmit_fb;
        for(int i=0; i<160; i++) {
          int offs = i % 40;
          if(offs >= 20) {
            offs = 19 - (offs - 20);
          }
          r.x = 20 + 2 + offs*30 - 2*(offs%2);
          r.y = 20 + 62 + (i / 20)*60;

          if(offs < 2 || offs > 17) {
            SDL_SetRenderDrawColor(C->renderer, *c++, *c++, *c++, 200);
          } else {
            SDL_SetRenderDrawColor(C->renderer, *c++, *c++, *c++, 255);
          }
          SDL_RenderFillRect(C->renderer, &r);
        }
        r.w = 56;
        r.h = 28;
        for(int i=0; i<160; i++) {
          int offs = i % 40;
          if(offs >= 20) {
            offs = 19 - (offs - 20);
          }
          r.y = 20 + 571 - (2 + offs*30 - 2*(offs%2));
          r.x = 20 + 62 + (i / 20)*60;

          if(offs < 2 || offs > 17) {
            SDL_SetRenderDrawColor(C->renderer, *c++, *c++, *c++, 200);
          } else {
            SDL_SetRenderDrawColor(C->renderer, *c++, *c++, *c++, 128);
          }
          SDL_RenderFillRect(C->renderer, &r);
        }
        SDL_RenderPresent(C->renderer);
      }
      
      SDL_LockMutex(C->mutex_wait_sensor);

      for(int i=0; i<SENSOR_ROWS; i++)
        C->sensors[C->measure_row * SENSOR_ROWS + i] = 100+(random()%120);

      SDL_CondSignal(C->cond_wait_sensor);
      SDL_UnlockMutex(C->mutex_wait_sensor);

      SDL_UnlockMutex(C->mutex_transmitting);
    } else {
      if(event.type == SDL_QUIT) {
        SDL_Quit();
      }
    }
  }
}

void bb_init_pos_led(bb_ctx* C) {
  memset(C->pos_led, 0xFF, 40*40*sizeof(int16_t));
  memset(C->pos_leds10, 0xFF, 10*10*4*sizeof(int16_t));
  for(int i=0; i<LED_COUNT; i++) {
    int x, y;
    bb_get_led_pos(C, i, &x, &y);

    C->pos_led[y][x] = i;

    int16_t *p10 = C->pos_leds10[y>>2][x>>2];
    while(*p10 != -1) p10++;
    *p10 = i;
  }
}

BB_API
int bb_open(bb_ctx **C) {
	*C = calloc(1, sizeof(bb_ctx));
	if(*C == NULL)
		return BB_MEMORY_ERROR;

  (*C)->mutex_wait_sensor = SDL_CreateMutex();
  if((*C)->mutex_wait_sensor == NULL)
		return BB_MEMORY_ERROR;
  (*C)->cond_wait_sensor = SDL_CreateCond();
  if((*C)->cond_wait_sensor == NULL)
		return BB_MEMORY_ERROR;
  (*C)->mutex_transmitting = SDL_CreateMutex();
  if((*C)->mutex_transmitting == NULL)
		return BB_MEMORY_ERROR;

  bb_init_pos_led(*C);

  char *bbemu = getenv("BBEMU");
  if(bbemu == NULL) {
    (*C)->ms_per_frame = TIME_PER_FRAME;
  } else {
    (*C)->ms_per_frame = strtoul(bbemu, NULL, 0);
    if((*C)->ms_per_frame == 0)
      (*C)->ms_per_frame = TIME_PER_FRAME;
  }

  if(NULL == ((*C)->event_thread = SDL_CreateThread(bb_event_thread, "Event Thread", (void*) *C))) {
    return BB_POLL_ERROR;
  }
	return 0;
}

BB_API
void bb_free(bb_ctx* C) {
	if(C != NULL) {
    if(C->running) {
      C->running = false;
      int status;
      SDL_WaitThread(C->event_thread, &status);
      (void)status;
      SDL_DestroyMutex(C->mutex_transmitting);
      SDL_DestroyMutex(C->mutex_wait_sensor);
      SDL_DestroyCond(C->cond_wait_sensor);
    }
		if(C->window != NULL) {
      SDL_DestroyWindow(C->window);
		}
		free(C);
	}
}

BB_API
int bb_transmit(bb_ctx *C, int measure_row) {
  SDL_LockMutex(C->mutex_transmitting);
  C->xmit = true;
  memcpy(C->xmit_fb, C->fb, LED_COUNT*3);
  C->measure_row = (measure_row == -1) ? ((C->measure_row + 1)%12) : measure_row;
  SDL_UnlockMutex(C->mutex_transmitting);
  return 0;
}

BB_API
void bb_get_led_pos(bb_ctx* C, int led, int* x, int* y) {
  if(led < 160) {
    // left-right
    *y = 5 + 4 * (led / 20);
    *x = 2 * (led % 20);
    if(((led / 20) % 2) == 1)
      *x = 38 - *x;
  } else {
    // down-up
    led -= 160;
    *x = 5 + 4 * (led / 20);
    *y = 2 * (led % 20);
    if(((led / 20) % 2) == 0)
      *y = 38 - *y;
  }
}

BB_API
void bb_set_led(bb_ctx *C, const int led, const uint8_t r, const uint8_t g, const uint8_t b) {
  uint8_t *p = C->fb + led*3;
  *p++ = g;
  *p++ = r;
  *p = b;
}

BB_API
void bb_set_led10(bb_ctx* C, const int x, const int y, const int r, const int g, const int b) {
  for(int i=0; i<4; i++) {
    int led = C->pos_leds10[y][x][i];
    if(led == -1) break;
    bb_set_led(C, led, r, g, b);
  }
}

BB_API
void bb_set_led40(bb_ctx* C, const int x, const int y, const int r, const int g, const int b) {
  int led = C->pos_led[y][x];
  if(led != -1)
    bb_set_led(C, led, r, g, b);
}

BB_API
int bb_wait_measure(bb_ctx* C) {
  SDL_LockMutex(C->mutex_wait_sensor);
  while(0 != SDL_CondWait(C->cond_wait_sensor, C->mutex_wait_sensor)) {
    // loop
  }
  int r = C->sensor_last_row;
  SDL_UnlockMutex(C->mutex_wait_sensor);
  return r;
}
