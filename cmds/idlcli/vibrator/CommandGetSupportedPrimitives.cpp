/*
 * Copyright (C) 2020 The Android Open Source Project *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "utils.h"
#include "vibrator.h"

namespace android {
namespace idlcli {

class CommandVibrator;

namespace vibrator {

using aidl::CompositePrimitive;

class CommandGetSupportedPrimitives : public Command {
    std::string getDescription() const override { return "List of supported effect primitive."; }

    std::string getUsageSummary() const override { return ""; }

    UsageDetails getUsageDetails() const override {
        UsageDetails details{};
        return details;
    }

    Status doArgs(Args &args) override {
        if (!args.empty()) {
            std::cerr << "Unexpected Arguments!" << std::endl;
            return USAGE;
        }
        return OK;
    }

    Status doMain(Args && /*args*/) override {
        auto hal = getHal();

        if (!hal) {
            return UNAVAILABLE;
        }

        std::vector<CompositePrimitive> primitives;
        auto status = hal->getSupportedPrimitives(&primitives);

        std::cout << "Status: " << status.getDescription() << std::endl;
        std::cout << "Primitives:" << std::endl;
        for (auto &e : primitives) {
            std::cout << "  " << toString(e) << std::endl;
        }

        return status.isOk() ? OK : ERROR;
    }
};

static const auto Command =
        CommandRegistry<CommandVibrator>::Register<CommandGetSupportedPrimitives>(
                "getSupportedPrimitives");

} // namespace vibrator
} // namespace idlcli
} // namespace android
