/*  Copyright (C) 2012-2020 by László Nagy
    This file is part of Bear.

    Bear is a tool to generate compilation database for clang tooling.

    Bear is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Bear is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "Application.h"
#include "librpc/InterceptClient.h"
#include "librpc/supervise.grpc.pb.h"
#include "er/Flags.h"
#include "libsys/Process.h"

#include <fmt/chrono.h>
#include <fmt/format.h>

#include <chrono>
#include <memory>

namespace {

    struct Execution {
        const std::string_view path;
        const std::vector<std::string_view> command;
        const std::string working_directory;
        const std::map<std::string, std::string> environment;
    };

    struct Session {
        const std::string_view destination;
    };

    rust::Result<Execution> make_execution(const ::flags::Arguments& args, const sys::Context& context) noexcept
    {
        auto path = args.as_string(::er::flags::EXECUTE);
        auto command = args.as_string_list(::er::flags::COMMAND);
        auto working_dir = context.get_cwd();
        auto environment = context.get_environment();

        return merge(path, command, working_dir)
            .map<Execution>([&environment](auto tuple) {
                const auto& [_path, _command, _working_dir] = tuple;
                return Execution { _path, _command, _working_dir, environment };
            });
    }

    rust::Result<Session> make_session(const ::flags::Arguments& args) noexcept
    {
        return args.as_string(er::flags::DESTINATION)
            .map<Session>([](const auto& destination) {
                return Session { destination };
            });
    }

    std::string now_as_string()
    {
        const auto now = std::chrono::system_clock::now();
        auto micros = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch());

        return fmt::format("{:%Y-%m-%dT%H:%M:%S}.{:06d}Z",
            fmt::localtime(std::chrono::system_clock::to_time_t(now)),
            micros.count() % 1000000);
    }

    std::shared_ptr<supervise::Event> make_start_event(
        pid_t pid,
        pid_t ppid,
        const Execution& execution)
    {
        std::shared_ptr<supervise::Event> result = std::make_shared<supervise::Event>();
        result->set_timestamp(now_as_string());

        std::unique_ptr<supervise::Event_Started> event = std::make_unique<supervise::Event_Started>();
        event->set_pid(pid);
        event->set_ppid(ppid);
        event->set_executable(execution.path.data());
        for (const auto& arg : execution.command) {
            event->add_arguments(arg.data());
        }
        event->set_working_dir(execution.working_directory);
        event->mutable_environment()->insert(execution.environment.begin(), execution.environment.end());

        result->set_allocated_started(event.release());
        return result;
    }

    std::shared_ptr<supervise::Event> make_stop_event(int status)
    {
        std::shared_ptr<supervise::Event> result = std::make_shared<supervise::Event>();
        result->set_timestamp(now_as_string());

        std::unique_ptr<supervise::Event_Stopped> event = std::make_unique<supervise::Event_Stopped>();
        event->set_status(status);

        result->set_allocated_stopped(event.release());
        return result;
    }
}

namespace er {

    struct Application::State {
        Session session;
        Execution execution;
    };

    rust::Result<Application> Application::create(const ::flags::Arguments& args, const sys::Context& context)
    {
        return rust::merge(make_session(args), make_execution(args, context))
            .map<Application>([&args, &context](auto in) {
                const auto& [session, execution] = in;
                auto state = new Application::State { session, execution };
                return Application(state);
            });
    }

    rust::Result<int> Application::operator()() const
    {
        rpc::InterceptClient client(impl_->session.destination);
        std::list<supervise::Event> events;

        auto result = client.get_environment_update(impl_->execution.environment)
            .and_then<sys::Process>([this](auto environment) {
                return sys::Process::Builder(impl_->execution.path)
                    .add_arguments(impl_->execution.command.begin(), impl_->execution.command.end())
                    .set_environment(environment)
                    .spawn(true);
            })
            .on_success([this, &events](auto& child) {
                // gRPC event update
                auto event_ptr = make_start_event(child.get_pid(), getppid(), impl_->execution);
                events.push_back(*event_ptr);
            })
            .and_then<int>([](auto child) {
                return child.wait();
            })
            .on_success([&events](auto exit) {
                // gRPC event update
                auto event_ptr = make_stop_event(exit);
                events.push_back(*event_ptr);
            });

        client.report(events);

        return result;
    }

    Application::Application(Application::State* const impl)
            : impl_(impl)
    {
    }

    Application::Application(Application&& rhs) noexcept
            : impl_(rhs.impl_)
    {
        rhs.impl_ = nullptr;
    }

    Application& Application::operator=(Application&& rhs) noexcept
    {
        if (&rhs != this) {
            delete impl_;
            impl_ = rhs.impl_;
        }
        return *this;
    }

    Application::~Application()
    {
        delete impl_;
        impl_ = nullptr;
    }
}
