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
};
