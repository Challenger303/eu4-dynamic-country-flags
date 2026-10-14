#ifndef CPBM_SCRIPT_H
#define CPBM_SCRIPT_H

/* Listens to EU4's own game.log for `cpbm_flag <TAG> <flagname>` lines written
 * by a script effect, and applies them. This is how conditions are evaluated:
 * by the engine, in its own scripting language, rather than by us reading
 * reverse-engineered structures out of memory. */
void cpbm_script_start(void);
int  cpbm_script_load_slots(void);

#endif
