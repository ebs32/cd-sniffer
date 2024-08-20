#pragma once

#include <stdbool.h>
#include <stdint.h>

enum kControllerStatus {
  STATUS_WAIT_FOR_POWER = 0,
  STATUS_IDLE,
  STATUS_RESET_IN_PROGRESS,
  STATUS_PICKUP_TO_INITIAL_POSITION,
};

/**
 * Returns the status of the controller.
 *
 * @returns The status of the controller.
 */
uint16_t ctl_get_status();

/**
 * Returns a friendly description of the status of the controller.
 *
 * @returns The friendly description of the status of the controller.
 */
const char* ctl_get_status_text();

/**
 * Checks if the controller is busy.
 *
 * The controller is busy when an operation is in progress. For example, moving
 * the optical pickup may take several seconds. During such time, the controller
 * won't accept any action.
 *
 * @returns TRUE, if the controller is busy; FALSE, otherwise.
 */
bool ctl_is_busy();

/**
 * Resets the controller.
 *
 * The XRST line is set to LOW for 1 second, then is set to HIGH. This action is
 * not mandatory and it is here just for testing purposes. Actually, the XRST
 * line must be hold LOW when there is no activity and HIGH when an operation is
 * in progress.
 */
void ctl_reset();

/**
 * Moves the optical pickup to the initial position.
 *
 * TODO
 */
void ctl_move_pickup_to_initial_position();
