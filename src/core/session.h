#pragma once

#include <cstddef>
#include <vector>

#include "image.h"

namespace eye {

// The set of loaded images and which one is showing.
class Session {
public:
    void add(ImagePtr img) {
        if (img) images_.push_back(std::move(img));
    }

    bool empty() const { return images_.empty(); }
    size_t size() const { return images_.size(); }

    const std::vector<ImagePtr>& images() const { return images_; }

    ImagePtr current() const {
        return images_.empty() ? nullptr : images_[current_];
    }
    size_t current_index() const { return current_; }

    void set_current(size_t i) {
        if (i < images_.size()) current_ = i;
    }

    void next() {
        if (!images_.empty()) current_ = (current_ + 1) % images_.size();
    }
    void prev() {
        if (!images_.empty())
            current_ = (current_ + images_.size() - 1) % images_.size();
    }

private:
    std::vector<ImagePtr> images_;
    size_t current_ = 0;
};

}  // namespace eye
