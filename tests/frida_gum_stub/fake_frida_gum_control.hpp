#pragma once

namespace fake_frida_gum {

void reset();
void reject_next_replace();
void fail_flush(bool fail);
int init_calls();
int replace_calls();
int revert_calls();
int unref_calls();
void* last_target();

} /// namespace fake_frida_gum
