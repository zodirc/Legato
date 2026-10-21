#ifndef LEGATO_ENGINE_CORPUS_HPP
#define LEGATO_ENGINE_CORPUS_HPP

namespace legato {

/* 0 表示命中并核对了内容哈希。非 0 时 err_name 是 IDL 错误名。 */
int resolve_corpus(const char* source_dir, const char* uri, char* path_out, int path_len, const char** err_name,
                   const char** err_message);

}  /* namespace legato */

#endif
