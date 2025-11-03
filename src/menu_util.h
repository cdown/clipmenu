#ifndef CM_MENU_UTIL_H
#define CM_MENU_UTIL_H

#include <stdint.h>

#include "config.h"

/**
 * Callback function type for handling selected clips.
 */
typedef int (*clip_action_fn)(struct config *cfg, uint64_t hash);

void menu_set_user_args(int argc, char **argv);

int _nonnull_ menu_prompt_and_act(struct config *cfg, const char *prompt,
                                  clip_action_fn action);

#endif
