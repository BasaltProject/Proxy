#pragma once

#include <basalt/proxy/runtime/BackendChangeRequest.hpp>
#include <basalt/wire/io/ResourceBudget.hpp>

#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>
#include <utility>

namespace basalt::proxy::runtime::detail {

struct BackendChangeState {
    BackendChangeState(wire::io::ResourceBudget::Reservation controlPermit,
                       wire::io::ResourceBudget::Reservation storagePermit)
        : control(std::move(controlPermit)), storage(std::move(storagePermit)) {}

    std::mutex publication;
    bool admitted = false;
    std::string player;
    std::string backend;
    std::atomic<std::size_t> remaining{0};
    std::atomic<bool> claimed{false};
    std::atomic<BackendChangeResult> outcome{BackendChangeResult::kPlayerNotFound};
    std::shared_ptr<std::atomic<bool>> stopped;
    wire::io::ResourceBudget::Reservation control;
    wire::io::ResourceBudget::Reservation storage;
};

} // namespace basalt::proxy::runtime::detail
