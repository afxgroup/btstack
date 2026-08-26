#include "bt_log.h"

/*
 * Off until -v turns it on. Every program built from this folder links this,
 * so a tool that never parses -v simply stays quiet.
 */
bool bt_log_enabled = false;
