#include "actions.h"
#include "controller.h"

const TAction actions[] = {
  {
    '0',
    "Reset",
    ctl_reset
  },
  {
    '1',
    "Move the optical pickup to the initial position",
    ctl_move_pickup_to_initial_position
  },
  {
    '2',
    "Move the optical pickup to the initial position then move it back",
    ctl_move_pickup_to_initial_position_then_move_it_back
  },
  {
    '3',
    "Test focus and tracking coils, and slide and spindle motors",
    ctl_run_test_coils_and_motors
  },
  {
    '4',
    "Play/Pause",
    ctl_play
  },
  {
    '5',
    "Stop",
    ctl_stop
  },
  {
    '6',
    "Tune tracking",
    ctl_tune_tracking
  },
};
