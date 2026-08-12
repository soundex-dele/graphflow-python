#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/standard_events.h"
#include "engine/graph_engine.h"

namespace py = pybind11;

namespace {

using graphflow::core::Blackboard;
using graphflow::core::EventBus;
using graphflow::core::GraphNode;
using graphflow::core::LifecycleState;
using graphflow::core::PayloadEvent;
using graphflow::engine::GraphEngine;
using graphflow::engine::NodeConfig;
using graphflow::engine::NodeCreator;

constexpr const char* kGlobalPythonEventTopic = "graphflow.python.global";

// One native carrier is enough for every event whose producer and consumers
// are Python nodes. It deliberately stays inside the binding: native nodes
// should use public, concrete EventT types such as PayloadEvent instead.
struct PythonEvent : graphflow::core::EventT<PythonEvent> {
    explicit PythonEvent(py::object value) : value(std::move(value)) {}
    py::object value;
};

nlohmann::json pythonToJson(const py::handle& value) {
    py::object json = py::module_::import("json");
    return nlohmann::json::parse(
        py::cast<std::string>(json.attr("dumps")(value)));
}

py::object jsonToPython(const nlohmann::json& value) {
    py::object json = py::module_::import("json");
    return json.attr("loads")(value.dump());
}

void reportPythonException(py::error_already_set& error,
                           const char* context) noexcept {
    try {
        error.discard_as_unraisable(context);
    } catch (...) {
        // A Python exception must never escape through a native graph thread.
    }
}

class PythonGraphNode;

class PythonNodeContext {
public:
    explicit PythonNodeContext(std::weak_ptr<PythonGraphNode> node)
        : node_(std::move(node)) {}

    const std::string& name() const;
    void subscribePython(py::object eventType, py::function callback);
    void subscribeGlobalPython(py::object eventType, py::function callback);
    void publishPython(py::object event, const std::string& condition = "");
    void subscribePayload(py::function callback);
    void publishPayload(const PayloadEvent& event,
                        const std::string& condition = "");
    bool isHalted() const;

private:
    std::shared_ptr<PythonGraphNode> requireNode() const;
    std::weak_ptr<PythonGraphNode> node_;
};

class PythonGraphNode final : public GraphNode,
                              public std::enable_shared_from_this<PythonGraphNode> {
public:
    explicit PythonGraphNode(const NodeConfig& config)
        : GraphNode(config.nodeName, config.eventBus, config.blackboard) {}

    ~PythonGraphNode() override {
        // GraphEngine clears paths while its Python wrapper deliberately has
        // the GIL released (so EventBus::drain cannot deadlock).  Dispose all
        // Python-owned members explicitly under the GIL before C++ member
        // destruction runs.
        py::gil_scoped_acquire gil;
        globalPythonSubscriptions_.clear();
        pythonSubscriptions_.clear();
        payloadCallbacks_.clear();
        instance_ = py::object();
    }

    void setInstance(py::object instance) { instance_ = std::move(instance); }

    void subscribePython(py::object eventType, py::function callback) {
        if (!PyType_Check(eventType.ptr())) {
            throw py::type_error("event_type must be a Python class");
        }
        pythonSubscriptions_.push_back(
            {std::move(eventType), std::move(callback)});
        if (pythonEventSubscribed_) return;

        pythonEventSubscribed_ = true;
        std::weak_ptr<PythonGraphNode> weak = shared_from_this();
        subscribe<PythonEvent>([weak](PythonEvent& event) {
            auto node = weak.lock();
            if (!node) return;
            py::gil_scoped_acquire gil;
            for (auto& subscription : node->pythonSubscriptions_) {
                try {
                    if (py::isinstance(event.value, subscription.eventType)) {
                        subscription.callback(event.value);
                    }
                } catch (py::error_already_set& error) {
                    reportPythonException(
                        error, "GraphFlow Python event callback");
                }
            }
        });
    }

    void publishPython(py::object event, const std::string& condition) {
        // Called from Python with the GIL held. GraphNode::publish is
        // synchronous, so every PythonEvent copy is also destroyed before
        // this call returns and therefore while the GIL is still held.
        publish<PythonEvent>(PythonEvent(std::move(event)), condition);
    }

    void subscribeGlobalPython(py::object eventType, py::function callback) {
        if (!PyType_Check(eventType.ptr())) {
            throw py::type_error("event_type must be a Python class");
        }
        if (!m_eventBus) {
            throw std::runtime_error("the node has no global EventBus");
        }
        globalPythonSubscriptions_.push_back(
            {std::move(eventType), std::move(callback)});
        if (globalPythonEventSubscribed_) return;

        globalPythonEventSubscribed_ = true;
        std::weak_ptr<PythonGraphNode> weak = shared_from_this();
        m_eventBus->subscribeAll<PythonEvent>(
            kGlobalPythonEventTopic, [weak](PythonEvent& event) {
                auto node = weak.lock();
                if (!node) return;
                py::gil_scoped_acquire gil;
                for (auto& subscription : node->globalPythonSubscriptions_) {
                    try {
                        if (py::isinstance(event.value,
                                           subscription.eventType)) {
                            subscription.callback(event.value);
                        }
                    } catch (py::error_already_set& error) {
                        reportPythonException(
                            error, "GraphFlow global Python event callback");
                    }
                }
            });
    }

    void subscribePayload(py::function callback) {
        const std::size_t index = payloadCallbacks_.size();
        payloadCallbacks_.push_back(std::move(callback));
        std::weak_ptr<PythonGraphNode> weak = shared_from_this();
        subscribe<PayloadEvent>([weak, index](PayloadEvent& event) {
            auto node = weak.lock();
            if (!node) return;
            py::gil_scoped_acquire gil;
            if (index >= node->payloadCallbacks_.size()) return;
            try {
                node->payloadCallbacks_[index](event);
            } catch (py::error_already_set& error) {
                reportPythonException(error, "GraphFlow PayloadEvent callback");
            }
        });
    }

    void publishPayload(const PayloadEvent& event,
                        const std::string& condition) {
        publish<PayloadEvent>(event, condition);
    }

protected:
    bool doInit() override { return callBool("on_init", true); }
    bool doStart() override { return callBool("on_start", true); }
    void doStop() override { callVoid("on_stop"); }
    void doRelease() override { callVoid("on_release"); }
    void doHalt() override { callVoid("on_halt"); }

private:
    struct PythonSubscription {
        py::object eventType;
        py::function callback;
    };

    bool callBool(const char* method, bool defaultValue) {
        py::gil_scoped_acquire gil;
        if (instance_.is_none() || !py::hasattr(instance_, method)) {
            return defaultValue;
        }
        try {
            py::object result = instance_.attr(method)();
            return result.is_none() ? defaultValue : py::cast<bool>(result);
        } catch (py::error_already_set& error) {
            reportPythonException(error, method);
            return false;
        }
    }

    void callVoid(const char* method) noexcept {
        py::gil_scoped_acquire gil;
        if (instance_.is_none() || !py::hasattr(instance_, method)) return;
        try {
            instance_.attr(method)();
        } catch (py::error_already_set& error) {
            reportPythonException(error, method);
        }
    }

    py::object instance_ = py::none();
    std::vector<PythonSubscription> globalPythonSubscriptions_;
    std::vector<PythonSubscription> pythonSubscriptions_;
    std::vector<py::function> payloadCallbacks_;
    bool pythonEventSubscribed_ = false;
    bool globalPythonEventSubscribed_ = false;
};

std::shared_ptr<PythonGraphNode> PythonNodeContext::requireNode() const {
    auto node = node_.lock();
    if (!node) throw std::runtime_error("the GraphFlow node has been released");
    return node;
}

const std::string& PythonNodeContext::name() const {
    return requireNode()->getName();
}

void PythonNodeContext::subscribePython(py::object eventType,
                                        py::function callback) {
    requireNode()->subscribePython(std::move(eventType), std::move(callback));
}

void PythonNodeContext::subscribeGlobalPython(py::object eventType,
                                              py::function callback) {
    requireNode()->subscribeGlobalPython(
        std::move(eventType), std::move(callback));
}

void PythonNodeContext::publishPython(py::object event,
                                      const std::string& condition) {
    requireNode()->publishPython(std::move(event), condition);
}

void PythonNodeContext::subscribePayload(py::function callback) {
    requireNode()->subscribePayload(std::move(callback));
}

void PythonNodeContext::publishPayload(const PayloadEvent& event,
                                       const std::string& condition) {
    requireNode()->publishPayload(event, condition);
}

bool PythonNodeContext::isHalted() const {
    return requireNode()->isHalted();
}

class PythonNodeCreator final : public NodeCreator {
public:
    explicit PythonNodeCreator(py::object factory)
        : factory_(std::move(factory)) {}

    std::shared_ptr<GraphNode> createNode(const NodeConfig& config) override {
        py::gil_scoped_acquire gil;
        auto node = std::make_shared<PythonGraphNode>(config);
        auto context = std::make_shared<PythonNodeContext>(node);
        py::object instance = factory_(context, jsonToPython(config.params));
        if (instance.is_none()) {
            throw std::runtime_error("Python node factory returned None");
        }
        node->setInstance(std::move(instance));
        return node;
    }

private:
    py::object factory_;
};

class NativePayloadSource final : public GraphNode {
public:
    explicit NativePayloadSource(const NodeConfig& config)
        : GraphNode(config.nodeName, config.eventBus, config.blackboard),
          payload_(config.params.value("payload", std::string())),
          sequence_(config.params.value("sequence", std::uint64_t{0})) {}

protected:
    bool doStart() override {
        publish<PayloadEvent>(PayloadEvent(payload_, sequence_));
        return true;
    }

private:
    std::string payload_;
    std::uint64_t sequence_;
};

class NativePayloadSink final : public GraphNode {
public:
    explicit NativePayloadSink(const NodeConfig& config)
        : GraphNode(config.nodeName, config.eventBus, config.blackboard),
          payloadKey_(config.params.value("payload_key", std::string("payload"))),
          sequenceKey_(config.params.value("sequence_key", std::string("sequence"))) {}

protected:
    bool doInit() override {
        subscribe<PayloadEvent>([this](PayloadEvent& event) {
            if (!m_blackboard) return;
            m_blackboard->set(payloadKey_, event.payload);
            m_blackboard->set(sequenceKey_, event.sequence);
        });
        return true;
    }

private:
    std::string payloadKey_;
    std::string sequenceKey_;
};

template <typename Node>
class SimpleNodeCreator final : public NodeCreator {
public:
    std::shared_ptr<GraphNode> createNode(const NodeConfig& config) override {
        return std::make_shared<Node>(config);
    }
};

class PythonGraphEngine {
public:
    PythonGraphEngine() = default;

    ~PythonGraphEngine() {
        // release() can drain native worker threads which may be waiting for
        // the GIL to enter a Python callback.
        py::gil_scoped_release release;
        engine_.release();
    }

    void registerPythonNode(const std::string& nodeClass, py::object factory) {
        if (!PyCallable_Check(factory.ptr())) {
            throw py::type_error("factory must be callable");
        }
        engine_.registerCreator(
            nodeClass, std::make_shared<PythonNodeCreator>(std::move(factory)));
    }

    void registerStandardPayloadNodes() {
        engine_.registerCreator(
            "NativePayloadSource",
            std::make_shared<SimpleNodeCreator<NativePayloadSource>>());
        engine_.registerCreator(
            "NativePayloadSink",
            std::make_shared<SimpleNodeCreator<NativePayloadSink>>());
    }

    bool load(const py::handle& config) {
        nlohmann::json value = pythonToJson(config);
        py::gil_scoped_release release;
        return engine_.loadFromJson(value);
    }

    bool reload(const py::handle& config) {
        nlohmann::json value = pythonToJson(config);
        py::gil_scoped_release release;
        return engine_.reloadFromJson(value);
    }

    bool loadFile(const std::string& path) {
        py::gil_scoped_release release;
        return engine_.loadFromJsonFile(path);
    }

    bool reloadFile(const std::string& path) {
        py::gil_scoped_release release;
        return engine_.reloadFromJsonFile(path);
    }

    bool init() {
        py::gil_scoped_release release;
        return engine_.init();
    }

    bool start() {
        py::gil_scoped_release release;
        return engine_.start();
    }

    void stop() {
        py::gil_scoped_release release;
        engine_.stop();
    }

    void release() {
        py::gil_scoped_release release;
        engine_.release();
    }

    void publishGlobal(py::object event) {
        if (engine_.state() == LifecycleState::Released) {
            throw std::runtime_error("cannot publish after engine release");
        }
        engine_.eventBus()->publish(
            kGlobalPythonEventTopic, PythonEvent(std::move(event)));
    }

    LifecycleState state() const { return engine_.state(); }
    std::size_t pathCount() const { return engine_.paths().size(); }

    py::object blackboardDict() {
        nlohmann::json value = engine_.blackboard()->toJson();
        if (value.is_null()) value = nlohmann::json::object();
        return jsonToPython(value);
    }

private:
    GraphEngine engine_;
};

}  // namespace

PYBIND11_MODULE(_graphflow_native, module) {
    module.doc() = "Native GraphFlow graph engine bindings";

    py::enum_<LifecycleState>(module, "LifecycleState")
        .value("CREATED", LifecycleState::Created)
        .value("INITIALIZED", LifecycleState::Initialized)
        .value("RUNNING", LifecycleState::Running)
        .value("STOPPED", LifecycleState::Stopped)
        .value("RELEASED", LifecycleState::Released);

    py::class_<PayloadEvent>(module, "PayloadEvent")
        .def(py::init<>())
        .def(py::init<std::string, std::uint64_t>(),
             py::arg("payload"), py::arg("sequence") = 0)
        .def_readwrite("payload", &PayloadEvent::payload)
        .def_readwrite("sequence", &PayloadEvent::sequence)
        .def_property_readonly("type_name", &PayloadEvent::typeName);

    py::class_<PythonNodeContext, std::shared_ptr<PythonNodeContext>>(
        module, "NodeContext")
        .def_property_readonly("name", &PythonNodeContext::name,
                               py::return_value_policy::copy)
        .def_property_readonly("is_halted", &PythonNodeContext::isHalted)
        .def("subscribe", &PythonNodeContext::subscribePython,
             py::arg("event_type"), py::arg("callback"))
        .def("subscribe_global", &PythonNodeContext::subscribeGlobalPython,
             py::arg("event_type"), py::arg("callback"))
        .def("publish", &PythonNodeContext::publishPython,
             py::arg("event"), py::arg("condition") = "")
        .def("subscribe_payload", &PythonNodeContext::subscribePayload,
             py::arg("callback"))
        .def("publish_payload", &PythonNodeContext::publishPayload,
             py::arg("event"), py::arg("condition") = "");

    py::class_<PythonGraphEngine>(module, "GraphEngine")
        .def(py::init<>())
        .def("register_python_node", &PythonGraphEngine::registerPythonNode,
             py::arg("node_class"), py::arg("factory"),
             py::keep_alive<1, 3>())
        .def("register_standard_payload_nodes",
             &PythonGraphEngine::registerStandardPayloadNodes)
        .def("load", &PythonGraphEngine::load, py::arg("config"))
        .def("load_file", &PythonGraphEngine::loadFile, py::arg("path"))
        .def("reload", &PythonGraphEngine::reload, py::arg("config"))
        .def("reload_file", &PythonGraphEngine::reloadFile, py::arg("path"))
        .def("init", &PythonGraphEngine::init)
        .def("start", &PythonGraphEngine::start)
        .def("stop", &PythonGraphEngine::stop)
        .def("release", &PythonGraphEngine::release)
        .def("publish_global", &PythonGraphEngine::publishGlobal,
             py::arg("event"))
        .def("__enter__", [](PythonGraphEngine& self) -> PythonGraphEngine& {
            return self;
        }, py::return_value_policy::reference_internal)
        .def("__exit__", [](PythonGraphEngine& self, py::object, py::object,
                            py::object) {
            self.release();
            return false;
        })
        .def_property_readonly("state", &PythonGraphEngine::state)
        .def_property_readonly("path_count", &PythonGraphEngine::pathCount)
        .def_property_readonly("blackboard", &PythonGraphEngine::blackboardDict);

    module.attr("PYTHON_EVENT_TYPE") = "PythonEvent";
}
