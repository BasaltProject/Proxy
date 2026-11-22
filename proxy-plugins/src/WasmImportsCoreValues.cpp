#include "WasmImports.hpp"
#include "WasmValues.hpp"

namespace basalt::proxy::plugins::wasmimport {
namespace {
using wire::wasm::kBasaltModule;
using wire::wasm::kErrAbsent;
using wire::wasm::kErrInvalid;
using wire::wasm::kErrStale;
using wire::wasm::kOk;

void valueResult(void* user, Call& call) {
    WasmPlugin& plugin = self(user);
    const std::int32_t ctx = call.i32(0);
    const WasmCall* live = plugin.host->live(ctx);
    if (live == nullptr) {
        return call.ret(ctx >= 1 && static_cast<std::uint32_t>(ctx) < plugin.host->nextCall_ ? kErrStale : kErrInvalid);
    }
    if (live->value == nullptr) {
        return call.ret(kErrInvalid);
    }
    call.ret(putValue(call.memory(), call.i32(1), call.i32(2), *live->value));
}

void requestCancel(void* user, Call& call) { call.ret(requests(self(user)).remove(call.i32(0)) ? kOk : kErrAbsent); }
} // namespace

void addCoreValuesImports(std::vector<Entry>& out) {
    out.push_back({kBasaltModule, "value_result", valueResult});
    out.push_back({kBasaltModule, "request_cancel", requestCancel});
}
} // namespace basalt::proxy::plugins::wasmimport
