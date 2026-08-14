#pragma once

#include <cstddef>
#include <vector>

#include "image.h"

namespace eye {

// The set of loaded images and which one is showing.
//
// A single-image viewer does not need a list. This is a list from the start
// because A/B comparison is the one feature we already know is coming, and
// retrofitting "there might be two images" through a renderer and a UI that
// assume one is exactly the kind of rewrite worth spending twenty lines to avoid.
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
