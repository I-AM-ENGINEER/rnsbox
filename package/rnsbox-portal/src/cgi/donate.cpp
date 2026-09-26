// donate.cpp — see donate.h. Addresses mirror the old donate.py exactly.
#include "donate.h"

namespace donate {

const Wallet WALLETS[] = {
    {"Bitcoin",  "BTC",  "₿", "bitcoin",
     "bc1q559qfr8nlqr2s6p07hgm03x357mncydehntdmlj7hu2qaud8wkqsgycagh"},
    {"Ethereum", "ETH",  "Ξ", "ethereum",
     "0x5bc9b408d67c4b8294290e1dd281526be5913864"},
    {"Solana",   "SOL",  "◎", "solana",
     "HjpiqhDdLd3p2ZcutYFptjX9TFiNYMWFGZUGVQnfesxM"},
    {"Litecoin", "LTC",  "Ł", "litecoin",
     "ltc1qaatf3kken6peg8z6s840w4kjad643y9gptt9775zgwkhpzuyxl3qjrpss2"},
    {"Dogecoin", "DOGE", "Ð", "dogecoin",
     "9umb1Mqvq5bYg7AG8fFghvzsFHZo9fnBmf"},
};

const int WALLET_COUNT = sizeof(WALLETS) / sizeof(WALLETS[0]);

}  // namespace donate
