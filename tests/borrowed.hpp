#pragma once

#include <memory>

// Non-owning shared_ptr for test fixture objects that outlive their users.
template <class T> std::shared_ptr<const T> borrowed(const T &value) {
    return {std::shared_ptr<const T>{}, &value};
}
