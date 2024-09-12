#pragma once

typedef void (*action_fn)();
typedef struct {
  const char      id;
  const char*     description;
  const action_fn fn;
} TAction;

// Contains all the actions implemented in the controller
const TAction actions[7];
