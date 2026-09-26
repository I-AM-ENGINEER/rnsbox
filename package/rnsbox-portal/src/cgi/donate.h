// donate.h — donation wallet addresses shown on the Donate page.
// Single source of truth (mirrors the old donate.py WALLETS). The QR SVGs
// under /www/qr/<ticker>.svg encode the bare address; `uri` builds the
// "open in wallet" scheme link (bitcoin:<addr> etc.).
#ifndef RNSBOX_DONATE_H
#define RNSBOX_DONATE_H

namespace donate {

struct Wallet {
    const char* name;
    const char* ticker;   // BTC / ETH / ...
    const char* symbol;   // ₿ / Ξ / ...
    const char* uri;      // scheme: bitcoin / ethereum / ...
    const char* address;
};

extern const Wallet WALLETS[];
extern const int WALLET_COUNT;

}  // namespace donate
#endif
