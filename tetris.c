/*********************************************************************
 * File: tetris.c                                                    *
 * Description: uses termbox2 to play tetris in the terminal         *
 * Author: Zachary E Graber (zachgraber27@gmail.com)                 *
 * GitHub: @zacharygraber (https://github.com/zacharygraber)         *
 * Created: 1/29/2023                                                *
 *********************************************************************/

/* TODO:
     * All pieces
     * Rotation
	 * Line clearing
     * Scoring and levels
*/

#define TB_IMPL

#include "termbox.h"
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <math.h>
#include <stdbool.h>
#include <pthread.h>

// Widths are effectively doubled, since a single "cell" is too skinny to be square
#define BOARD_WIDTH 10 // MAX OF 255 
#define BOARD_HEIGHT 20 // MAX OF 255
#define MIN_WIDTH (BOARD_WIDTH + 2)*2
#define MIN_HEIGHT (BOARD_HEIGHT + 2)

// A game piece (a tetromino) is made of 4 blocks
typedef struct block_t {
	uint8_t x;
	uint8_t y;
} block_t;

typedef struct piece_t {
	block_t blocks[4];
	uintattr_t color;
} piece_t;

typedef enum direc_t {
	LEFT,
	RIGHT,
	DOWN // pieces can never move up
} direc_t;

typedef enum game_state_t {
	PLAY,
	PAUSE,
	GAME_OVER,
	QUIT
} game_state_t;

// Globals ///////////
static double drop_speed = 1000.0; // (in ms) start by moving piece down every second
static uintattr_t board[BOARD_WIDTH][BOARD_HEIGHT]; // board is 2D grid of colors (the active piece is NOT part of the board)
static piece_t active_piece = {0};
static pthread_mutex_t board_mutex, active_piece_mutex;
static pthread_t event_handler_pt;
static game_state_t GAME_STATE = PAUSE;
//////////////////////

void initialize();
void pause_game();
void resume_game();
void game_over();
void setup_new_game();
void *event_handler_pthread_routine(void *args);
void render();
void draw_block(int x, int y, uintattr_t color);
void show_321_countdown();
void create_new_active_piece();
bool move_active_piece(direc_t d);
void hard_drop_active_piece();
void settle_active_piece();
void sigint_handler(int sig);
void quit(int status, const char *exit_msg);

int main(void) {
	tb_init();
	initialize();
	
	double remaining_wait_time_ms = drop_speed, loop_time_taken_ms = 0;
    struct timespec sleep_ts;
    clock_t start, stop;
    while (true) {
        switch (GAME_STATE) {
            case PLAY:
                start = clock();
                // Move piece down and re-render to display change (rendering handled in move)
                move_active_piece(DOWN);
                stop = clock();

                loop_time_taken_ms = ((double) (stop - start) * 1000.0) / CLOCKS_PER_SEC;
                remaining_wait_time_ms = drop_speed - loop_time_taken_ms;

				// Sleep if necessary
                if (remaining_wait_time_ms > 0) {
                    sleep_ts.tv_sec = ((long) remaining_wait_time_ms) / 1000; // cast is safe to truncate since guaranteed > 0
                    sleep_ts.tv_nsec = (long) (fmod(remaining_wait_time_ms, 1000.0) * 1000000);
                    nanosleep(&sleep_ts, &sleep_ts);
                }
                break;

            case GAME_OVER:
                // Wait for input
                break;

            case PAUSE:
                // Nothing to do except wait for input
                break;

            case QUIT:
                quit(EXIT_SUCCESS, "Game over!");
                break;
        }
    }
	
	// We should never hit this. Any exit should happen through quit().
	exit(EXIT_FAILURE);
}

/* initializes the resources and threads needed to run the game
 */
void initialize() {
	// handle inadequate window dimensions
	if ((tb_width() < MIN_WIDTH) || (tb_height() < MIN_HEIGHT)) {
		quit(EXIT_FAILURE, "Window dimensions are too small!");
	}

	// Register sigint handler to gracefully shut down
	signal(SIGINT, sigint_handler);


	// Create mutexes for board and active piece
	if (pthread_mutex_init(&board_mutex, NULL) || pthread_mutex_init(&active_piece_mutex, NULL)) {
		quit(EXIT_FAILURE, "Couldn't initialize mutex");
	}


	// Spawn pthread for main event handler
	if (pthread_create(&event_handler_pt, NULL, event_handler_pthread_routine, NULL)) {
		quit(EXIT_FAILURE, "Failed to create pthread for main loop");
	}

	setup_new_game();

	// Make a call to render() the first frame
	render();

	resume_game();
	return;
}

void pause_game() {
	if (GAME_STATE == PAUSE) return;
	GAME_STATE = PAUSE;
	return;
}

void resume_game() {
	if (GAME_STATE == PLAY) return;

	show_321_countdown();
	render();
	GAME_STATE = PLAY;
	return;
}

void game_over() {
	GAME_STATE = GAME_OVER;
	tb_print(10, 8, TB_WHITE, TB_RED, "GAME");
    tb_print(10, 9, TB_WHITE, TB_RED, "OVER");
    tb_print(11, 11, TB_WHITE, TB_RED, ":(");
    tb_present();
	return;
}

// Sets the board to all black and creates a fresh active piece
void setup_new_game() {
	pthread_mutex_lock(&board_mutex);
	// Initialize the board to all black
    for (uint8_t i = 0; i < BOARD_WIDTH; i++) {
        for (uint8_t j = 0; j < BOARD_HEIGHT; j++) {
            board[i][j] = TB_BLACK;
        }
    }
	pthread_mutex_unlock(&board_mutex);

	create_new_active_piece();
}

/* Routine for a pthread to execute. As long as this pthread is alive, its loop will
 * continually poll termbox for events (including keyboard input) and handle them appropriately.
 * Intended to stay alive as long as the program does.
 * 
 * Set GAME_STATE to QUIT to signal for this routine to exit
 */
void *event_handler_pthread_routine(void *args) {
	(void)args; // Surpress unused parameter warning
	struct tb_event event = {0};
	while (GAME_STATE != QUIT) {
		// Handle keyboard input
        tb_poll_event(&event);
        if (event.type == TB_EVENT_KEY) {
			switch (GAME_STATE) {
				case PLAY:
					switch (event.key) {
						case TB_KEY_CTRL_C:
						case TB_KEY_ESC:
							GAME_STATE = QUIT;
							break;
						case TB_KEY_ARROW_LEFT:
							move_active_piece(LEFT);
							break;
						case TB_KEY_ARROW_RIGHT:
							move_active_piece(RIGHT);
							break;
						case TB_KEY_ARROW_DOWN:
							move_active_piece(DOWN);
							break;
						case TB_KEY_SPACE:
							hard_drop_active_piece();
							break;
					}
					switch (event.ch) {
						case 'p':
						case 'P':
							pause_game();
							break;
						case ' ':
							hard_drop_active_piece();
							break;
					}
					break;

				case GAME_OVER:
					switch (event.key) {
						case TB_KEY_ENTER:
							setup_new_game();
							render();
							resume_game();
							break;
						case TB_KEY_CTRL_C:
                        case TB_KEY_ESC:
                            GAME_STATE = QUIT;
                            break;
					}
					break;

				case PAUSE:
					switch (event.ch) {
                        case 'p':
                        case 'P':
                            resume_game();
                            break;
                    }
			}
		}
	}
	pthread_exit(NULL);
}

/* draws a square(ish) block of `color` at x,y in GAME GRID COORDINATES */
void draw_block(int x, int y, uintattr_t color) {
	// x coordinate is doubled since each "block" is 2 chars wide
	tb_print(2 * (x+1), y+1, color, 0, "██"); // add one to account for frame
}

void show_321_countdown() {
	// 3
	draw_block(3,5,TB_RED); draw_block(4,5,TB_RED); draw_block(5,5,TB_RED);
	draw_block(6,6,TB_RED); draw_block(6,7,TB_RED); draw_block(6,8,TB_RED);
	draw_block(4,9,TB_RED); draw_block(5,9,TB_RED); draw_block(6,10,TB_RED);
	draw_block(6,11,TB_RED); draw_block(6,12,TB_RED); draw_block(6,13,TB_RED);
	draw_block(3,14,TB_RED); draw_block(4,14,TB_RED); draw_block(5,14,TB_RED);
	tb_present();
	sleep(1);

	// 2
	for (uint8_t i = 2; i < BOARD_WIDTH; i++) {
        for (uint8_t j = 3; j < BOARD_HEIGHT; j++) {
            draw_block(i, j, TB_BLACK);
        }
    }
	draw_block(3,6,TB_YELLOW); draw_block(4,6,TB_YELLOW); draw_block(5,6,TB_YELLOW);
	draw_block(6,7,TB_YELLOW); draw_block(6,8,TB_YELLOW); draw_block(6,9,TB_YELLOW);
	draw_block(5,10,TB_YELLOW); draw_block(4,11,TB_YELLOW); draw_block(3,12,TB_YELLOW);
	draw_block(3,13,TB_YELLOW); draw_block(4,13,TB_YELLOW); draw_block(5,13,TB_YELLOW);
	draw_block(6,13,TB_YELLOW);
	tb_present();
	sleep(1);

	// 1
	for (uint8_t i = 2; i < BOARD_WIDTH; i++) {
        for (uint8_t j = 3; j < BOARD_HEIGHT; j++) {
            draw_block(i, j, TB_BLACK);
        }
    }
	draw_block(5,5,TB_GREEN); draw_block(5,6,TB_GREEN); draw_block(4,6,TB_GREEN);
	draw_block(3,7,TB_GREEN); draw_block(4,7,TB_GREEN); draw_block(5,7,TB_GREEN);
	draw_block(4,8,TB_GREEN); draw_block(5,8,TB_GREEN); draw_block(4,9,TB_GREEN); 
	draw_block(5,9,TB_GREEN); draw_block(4,10,TB_GREEN); draw_block(5,10,TB_GREEN);
	draw_block(4,11,TB_GREEN); draw_block(5,11,TB_GREEN); draw_block(4,12,TB_GREEN);
	draw_block(5,12,TB_GREEN); draw_block(4,13,TB_GREEN); draw_block(5,13,TB_GREEN);
	draw_block(3,14,TB_GREEN); draw_block(4,14,TB_GREEN); draw_block(5,14,TB_GREEN);
	draw_block(6,14,TB_GREEN);
	tb_present();
	sleep(1);
}

/* Re-renders everything on the screen
 */
// SHOULD BE [MOSTLY] THREAD SAFE
void render() {
	tb_clear();
	// The order in which things get rendered is important!
	// Draw the outside frame of the board
    for (int i = -1; i <= BOARD_WIDTH; i++) {
        draw_block(i, -1, TB_WHITE); // Top row
        draw_block(i, BOARD_HEIGHT, TB_WHITE); // Bottom row
    }
    for (int i = 0; i < BOARD_HEIGHT; i++) {
        draw_block(-1, i, TB_WHITE); // left
        draw_block(BOARD_WIDTH, i, TB_WHITE); // right
    }

    // Draw the board
	pthread_mutex_lock(&board_mutex);
    for (uint8_t i = 0; i < BOARD_WIDTH; i++) {
        for (uint8_t j = 0; j < BOARD_HEIGHT; j++) {
            draw_block(i, j, board[i][j]);
        }
    }
	pthread_mutex_unlock(&board_mutex);

    // Draw active piece
	pthread_mutex_lock(&active_piece_mutex);
    for (uint8_t i = 0; i < 4; i++) {
        draw_block(active_piece.blocks[i].x, active_piece.blocks[i].y, active_piece.color);
    }
	pthread_mutex_unlock(&active_piece_mutex);
	tb_present();
    return;
}

// THREAD SAFE
void create_new_active_piece() {
	pthread_mutex_lock(&active_piece_mutex);

	// TODO: implement actual tetrominos
	active_piece.color = TB_CYAN;
	active_piece.blocks[0].x = 3;
	active_piece.blocks[0].y = 0;
	active_piece.blocks[1].x = 4;
	active_piece.blocks[1].y = 0;
	active_piece.blocks[2].x = 5;
	active_piece.blocks[2].y = 0;
	active_piece.blocks[3].x = 6;
	active_piece.blocks[3].y = 0;

	// Check to make sure the new piece isn't on top of any existing "settled" ones
	// If so, it's a game over.
	bool game_over_happens = false;
	pthread_mutex_lock(&board_mutex);
	for (uint8_t i = 0; i < 4; i++) {
		if (board[active_piece.blocks[i].x][active_piece.blocks[i].y] != TB_BLACK) {
			game_over_happens = true;
		}
	}
	pthread_mutex_unlock(&board_mutex);
	pthread_mutex_unlock(&active_piece_mutex);
	
	render();
	if (game_over_happens) game_over();
}

/* Moves the active piece in the direction specified
 * returns true if the piece is still in play after the move
 * returns false if the piece "settled" on the board after the move
 */
// THREAD SAFE
bool move_active_piece(direc_t d) {
	block_t new_blocks[4];
	int new_x=0, new_y=0;

	pthread_mutex_lock(&active_piece_mutex);
	pthread_mutex_lock(&board_mutex); // not writing to board, but want to make sure it doesn't change
	// Check for each block in the piece if the new positions are valid
	for (uint8_t i = 0; i < 4; i++) {
		switch (d) {
			case LEFT:
				new_x = active_piece.blocks[i].x - 1;
                new_y = active_piece.blocks[i].y; // y doesn't change

                // Make sure moving left wouldn't put us through the left wall or in an already occupied space
                if (new_x < 0 || board[new_x][new_y] != TB_BLACK) {
					pthread_mutex_unlock(&board_mutex);
					pthread_mutex_unlock(&active_piece_mutex);
                    return true; // This is a "valid" move, but the piece doesn't change positions
                }
				break;

			case RIGHT:
				new_x = active_piece.blocks[i].x + 1;
                new_y = active_piece.blocks[i].y; // y doesn't change
                if (new_x >= BOARD_WIDTH || board[new_x][new_y] != TB_BLACK) {
					pthread_mutex_unlock(&board_mutex);
                    pthread_mutex_unlock(&active_piece_mutex);
                    return true; // This is a "valid" move, but the piece doesn't change positions
                }
				break;

			case DOWN:
				new_x = active_piece.blocks[i].x; // x doesn't change
                new_y = active_piece.blocks[i].y + 1;
				if (new_y >= BOARD_HEIGHT || board[new_x][new_y] != TB_BLACK) {
					pthread_mutex_unlock(&board_mutex);
                    pthread_mutex_unlock(&active_piece_mutex);
					settle_active_piece(); // Render is handled by this function call
					return false; // Piece hit the bottom of the board or another "settled" piece
				}
				break;
		}
		new_blocks[i].x = new_x;
    	new_blocks[i].y = new_y;
	}
    pthread_mutex_unlock(&board_mutex);

	// All guard clauses passed: move to new positions
	for (uint8_t i = 0; i < 4; i++) {
		active_piece.blocks[i] = new_blocks[i];
	}
	pthread_mutex_unlock(&active_piece_mutex);
	render();
	return true;
}

void hard_drop_active_piece() {
	while (move_active_piece(DOWN)) {
		continue;
	}
	return;
} 

/* "Settles" the active piece by writing its current position
 * to the board and creating a new active piece
 */
// THREAD SAFE
void settle_active_piece() {
	pthread_mutex_lock(&active_piece_mutex);
    pthread_mutex_lock(&board_mutex);
	for (uint8_t i = 0; i < 4; i++) {
		board[active_piece.blocks[i].x][active_piece.blocks[i].y] = active_piece.color;
	}
	pthread_mutex_unlock(&active_piece_mutex);
    pthread_mutex_unlock(&board_mutex);
	
	// Render is handled by this call
	create_new_active_piece();
}

void sigint_handler(int sig) {
	(void)sig; // Surpress unused parameter warning
	quit(EXIT_FAILURE, "Received SIGINT");
}

/* Quit the game gracefully, releasing any memory
 * and shutting down termbox2
 * Takes in an exit status and a message to print to stdout/stderr
 */
void quit(int status, const char *exit_msg) {
	// Signal for pthread to exit, then wait for it
	GAME_STATE = QUIT;
	pthread_join(event_handler_pt, NULL);
	
	// unlock them first, just in case
	pthread_mutex_unlock(&board_mutex);
    pthread_mutex_destroy(&board_mutex);
	pthread_mutex_unlock(&active_piece_mutex);
    pthread_mutex_destroy(&active_piece_mutex);
	
	tb_shutdown();
	fprintf((status == EXIT_SUCCESS) ? stdout : stderr, "Tetris exited: %s\n", exit_msg);
	exit(status);
}
