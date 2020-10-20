#include "common.h"
#include "dummy.h"
#include "vault.h"
#include "Math.h"

// Demonstration of the inter-shader interaction.

static const ContractID g_cidVault = {
    0xc5,0x7c,0x6d,0x7e,0x0b,0xeb,0x15,0xd2,0x37,0x92,0xbe,0x1d,0xad,0x64,0xcb,0x9f,0xaa,0xb8,0x38,0x6b,0xb6,0xf3,0x5f,0x44,0xe0,0xa9,0xdc,0xd9,0x86,0xad,0xd2,0x43,
};


export void Ctor(void*)
{
    uint8_t ok = Env::RefAdd(g_cidVault);
    Env::Halt_if(!ok); // if the target shader doesn't exist the VM still gives a chance to run, but we don't need it.
}
export void Dtor(void*)
{
    Env::RefRelease(g_cidVault);
}

export void Method_2(void*)
{
    Vault::Deposit r;
    Utils::ZeroObject(r);
    r.m_Amount = 318;
    Env::CallFar_T(g_cidVault, r);
}

export void Method_3(Dummy::MathTest1& r)
{
    auto res =
        MultiPrecision::From(r.m_Value) *
        MultiPrecision::From(r.m_Rate) *
        MultiPrecision::From(r.m_Factor);

    //    Env::Trace("res", &res);

    auto trg = MultiPrecision::FromEx<2>(r.m_Try);

    //	Env::Trace("trg", &trg);

    r.m_IsOk = (trg <= res);
}
