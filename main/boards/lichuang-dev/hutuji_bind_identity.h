#ifndef HUTUJI_BIND_IDENTITY_H
#define HUTUJI_BIND_IDENTITY_H

#include "hutuji_bind_identity_core.h"

namespace hutuji {

// 开始新的本地扫码会话；落盘成功后才允许显示二维码。
bool BeginBindIdentitySession(BindIdentitySession& session);
// 配网重启后复用同一码；没有记录时返回 false，不静默生成。
bool LoadBindIdentitySession(BindIdentitySession& session);
// 只有完成的 nonce 仍属于当前会话才清除，迟到回包不能清掉新二维码。
bool FinishBindIdentitySession(const std::string& nonce);
bool SignBindIdentity(const BindIdentitySession& session, const std::string& challenge,
                      const std::string& mac, const std::string& sku, const std::string& token,
                      std::string& signature);

}  // namespace hutuji

#endif
