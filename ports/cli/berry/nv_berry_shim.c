// No shell on NucleoOS: os.system() reports failure (-1) instead of failing to link.
int system(const char *cmd) { (void)cmd; return -1; }
