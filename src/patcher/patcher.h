#pragma once

/**
 * Runs the patcher.
 *
 * The patcher performs a man-in-the-middle attack and sets the tracking values
 * to the ones set by the user. If a value is set to zero then the patcher will
 * do nothing. For this reason, if the tracking balance is set to zero then the
 * adjust algorithm will be executed. Same goes for the tracking gain.
 *
 * Other MICOM commands are sent without any modification.
 *
 * On the other hand, if the switch on the PCB is pressed for a second then the
 * WiFi interface will be enabled so the user can connect to it for setting the
 * values and/or running MICOM commands.
 */
void run_patcher();
