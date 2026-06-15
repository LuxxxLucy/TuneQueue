#ifndef TUNE_QUEUE_HELPER_H
#define TUNE_QUEUE_HELPER_H

// join sub under $HOME (or "." when HOME is unset)
void home_join(const char *sub, char *out, int n);

void mkdir_p(const char *dir);
unsigned char *read_file(const char *path, int *len);  // malloc'd, or NULL

#endif
