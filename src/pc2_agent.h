/*
  pc2_agent: background enabler. Watches for controller attach and system
  wake, and re-sends the init sequence each time.
*/
#ifndef PC2_AGENT_H
#define PC2_AGENT_H

#define PC2_LOG_SUBSYSTEM   "io.github.procon2-usb"

/* Runs until terminated. `mapping_path` may be NULL; if set, it's published
   as SDL_GAMECONTROLLERCONFIG_FILE for apps launched in this login session.
   Returns a process exit code. */
int PC2_AgentRun(const char *mapping_path);

#endif /* PC2_AGENT_H */
