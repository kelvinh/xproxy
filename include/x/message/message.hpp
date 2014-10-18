#ifndef MESSAGE_HPP
#define MESSAGE_HPP

namespace x {
namespace message {

class message {
public:
    DEFAULT_DTOR(message);

    virtual bool deliverable() = 0;
};

} // namespace message
} // namespace x

#endif // MESSAGE_HPP
