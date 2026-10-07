#pragma once

#include <QString>

namespace Acheron {
namespace Discord {

struct HttpResponse;

struct ApiError
{
    static constexpr int UnknownInvite = 10006;
    static constexpr int TooManyEmojis = 30008;
    static constexpr int TooManyReactions = 30010;
    static constexpr int TooManyAnimatedEmojis = 30018;
    static constexpr int InvalidFormBody = 50035;
    static constexpr int FileTooLarge = 50045;
    static constexpr int CannotResizeAnimated = 50138;

    int code = 0;
    QString message;

    static ApiError fromResponse(const HttpResponse &response);
};

} // namespace Discord
} // namespace Acheron
