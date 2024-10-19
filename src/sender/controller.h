#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  bool        is_busy;      // Indicates if the controller is busy
  bool        is_powered;   // Indicates if the controller is powered
  const char* status_text;  // Friendly description of the current status
} TEvent;

// Signature of the callback function to call on an event
typedef void (*ctl_listener_t)(const TEvent*);

/**
 * Initializes the controller.
 *
 * This API must be called before any other API. Otherwise, the behaviour is
 * undefined.
 */
void ctl_start();

/**
 * Registers a new listener.
 *
 * Once registered, the listener will be notified with the current status of the
 * controller.
 *
 * @returns 0, on success; -1, if no more listeners can be registered.
 */
int32_t ctl_add_listener(const ctl_listener_t);

/**
 * Resets the controller.
 *
 * This action stops all the servos and cancels any auto-sequence command. The
 * reset line is set low so the system is in RESET state.
 */
void ctl_reset();

/**
 * Moves the optical pickup to the initial position.
 *
 * Once the action is completed the reset line is set low so the system is in
 * RESET state.
 */
void ctl_move_pickup_to_initial_position();

/**
 * Moves the optical pickup to the initial position and then moves it backwards
 * for a period of time.
 *
 * Once the action is completed the reset line is set low so the system is in
 * RESET state.
 */
void ctl_move_pickup_to_initial_position_then_move_it_back();

/**
 * Runs a few mechanical tests.
 *
 * This action tests the focus and tracking coils and the sled and spindle
 * motors. Once the action is completed the reset line is set low so the system
 * is in RESET state.
 */
void ctl_run_test_coils_and_motors();

/**
 * Plays a disc from the current optical pickup position.
 *
 * The user must use STOP or RESET to finish this action. Otherwise, the optical
 * pickup may be pushed against the chassis.
 *
 * This API can be used to pause the disc playing too. If the API is called when
 * a disc is being played it will pause the reproduction. To resume it, call
 * this API again.
 */
void ctl_play();

/**
 * Stops playing a disc.
 *
 * This API will cancel the PLAY action and move the optical pickup to the
 * initial position.
 */
void ctl_stop();

/**
 * Sets the environment for tuning the tracking balance and gain.
 *
 * After calling this API, the user should run MICOM commands to tune the
 * tracking.
 *
 * The command "84x" will set the balance window level. Once this command is
 * completed, the user should try different balance settings until a condition
 * is met.
 *
 * Once the balance is set, the user should proceed with the gain following a
 * similar approach. Please refer to the data sheet for more information.
 */
void ctl_tune_tracking();

/**
 * Runs arbitrary MICOM commands.
 *
 * Because this API runs asynchronously the caller must not release the buffer.
 * Instead, this API will take care of release the buffer once all the commands
 * have been processed.
 *
 * A default delay is added between commands.
 */
void ctl_run_micom_commands(size_t n, uint16_t* commands);
