
namespace lldb::imgui {

/// Unlike `dlsym` this function can resolve internal symbols in `LLDB.framework`
void* ResolvePrivateSymbol(const char* symbol);

}
