// genra_ui.h - the screen of a GENRA plugin on the MPC: texts, film strips, buttons that flash.
//
// SPDX-License-Identifier: GPL-3.0-only
//
// The MPC draws what the skin binds to parameters. Fixed texts in a skin are not drawn, so every text (captions
// too) is a read-only parameter the plugin serves through formatValue(). The MPC re-reads a text only when it thinks
// the value changed: push() nudges every changed text parameter, gives every film strip its frame as the value,
// tells the host (audioMasterAutomate) and asks for a redraw (audioMasterUpdateDisplay).
//
// Buttons are bool parameters the skin flips on every tap. The MPC also probes values to read texts, so a change
// counts as a tap only when it stayed for two looks of the worker (>= 40 ms): taps() returns those. A button can
// have a flash: a 2-frame film strip behind it that lights for 350 ms after a tap.
// A text can show a message for a moment (2 s) in place of its usual text.
//
// setText / setFrame / taps / push: the worker thread. format: the UI thread. Texts are cut after 31 bytes
// (the SDK's limit).

#ifndef GENRA_UI_H
#define GENRA_UI_H

#include "mpc_plugin.h"

#include <chrono>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace genraui {

class Screen
{
public:
    using Clock = std::chrono::steady_clock;
    explicit Screen(mpc::Plugin& p) : p_(p) {}

    void init()
    {
        int n = p_.numParams();
        kind_.assign((size_t)n, 0);
        frames_.assign((size_t)n, 1);
        texts_.assign((size_t)n, std::string());
        msg_.assign((size_t)n, std::string());
        msgUntil_.assign((size_t)n, Clock::time_point{});
        shown_.assign((size_t)n, std::string("\x01"));
        frame_.assign((size_t)n, 0);
        frameShown_.assign((size_t)n, -1);
        flashOf_.assign((size_t)n, -1);
        flashUntil_.assign((size_t)n, Clock::time_point{});
        seen_.assign((size_t)n, 0);
        prev_.assign((size_t)n, 0);
    }
    void text(int i, const std::string& initial = std::string()) { kind_[(size_t)i] = 1; texts_[(size_t)i] = fit(initial); }
    void strip(int i, int frames) { kind_[(size_t)i] = 2; frames_[(size_t)i] = frames < 2 ? 2 : frames; }
    void button(int i, int flashStrip = -1)
    {
        kind_[(size_t)i] = 3;
        flashOf_[(size_t)i] = flashStrip;
        if (flashStrip >= 0) strip(flashStrip, 2);
        seen_[(size_t)i] = prev_[(size_t)i] = p_.getInt(i);
    }
    // also watched for taps (LISTE, a choice the plugin must react to at once ...): counted like a button
    void watch(int i) { kind_[(size_t)i] = 4; seen_[(size_t)i] = prev_[(size_t)i] = p_.getInt(i); }
    bool isText(int i) const { return i >= 0 && i < (int)kind_.size() && kind_[(size_t)i] == 1; }

    static std::string fit(const std::string& s, bool keepEnd = false)
    {
        if (s.size() <= 31) return s;
        return keepEnd ? "..." + s.substr(s.size() - 28) : s.substr(0, 30) + "~";
    }
    void setText(int i, const std::string& s, bool keepEnd = false)
    {
        std::lock_guard<std::mutex> g(mtx_);
        texts_[(size_t)i] = fit(s, keepEnd);
    }
    void message(int i, const std::string& s, int ms = 2000)
    {
        std::lock_guard<std::mutex> g(mtx_);
        msg_[(size_t)i] = fit(s);
        msgUntil_[(size_t)i] = Clock::now() + std::chrono::milliseconds(ms);
    }
    void setFrame(int i, int f) { frame_[(size_t)i] = f; }
    void flash(int buttonParam)
    {
        int f = flashOf_[(size_t)buttonParam];
        if (f >= 0) flashUntil_[(size_t)f] = Clock::now() + std::chrono::milliseconds(350);
    }

    // UI thread
    bool format(int i, std::string& out)
    {
        if (i < 0 || i >= (int)kind_.size() || kind_[(size_t)i] != 1) return false;
        std::lock_guard<std::mutex> g(mtx_);
        out = Clock::now() < msgUntil_[(size_t)i] ? msg_[(size_t)i] : texts_[(size_t)i];
        return true;
    }

    // a project or the host set the buttons: that is no tap
    void resync()
    {
        for (size_t i = 0; i < kind_.size(); ++i)
            if (kind_[i] >= 3) seen_[i] = prev_[i] = p_.getInt((int)i);
    }

    // the buttons tapped (and watched parameters turned) since the last look: (parameter, change)
    std::vector<std::pair<int, int>> taps()
    {
        std::vector<std::pair<int, int>> out;
        for (size_t i = 0; i < kind_.size(); ++i)
        {
            if (kind_[i] < 3) continue;
            int v = p_.getInt((int)i);
            if (v != seen_[i] && v == prev_[i])
            {
                out.push_back({(int)i, v - seen_[i]});
                seen_[i] = v;
                if (kind_[i] == 3) flash((int)i);
            }
            prev_[i] = v;
        }
        return out;
    }

    void push()
    {
        bool any = false;
        const auto now = Clock::now();
        for (size_t i = 0; i < kind_.size(); ++i)
        {
            if (kind_[i] == 1)
            {
                std::string t;
                {
                    std::lock_guard<std::mutex> g(mtx_);
                    t = now < msgUntil_[i] ? msg_[i] : texts_[i];
                }
                if (t == shown_[i]) continue;
                shown_[i] = t;
                p_.setNormalised((int)i, p_.getNormalised((int)i) > 0.5f ? 0.f : 1.f);
            }
            else if (kind_[i] == 2)
            {
                int f = frame_[i];
                if (flashUntil_[i] != Clock::time_point{}) f = now < flashUntil_[i] ? 1 : 0;
                if (f == frameShown_[i]) continue;
                frameShown_[i] = f;
                p_.setNormalised((int)i, (float)f / (float)(frames_[i] - 1));
            }
            else continue;
            p_.notifyHostParameterChanged((int)i);
            any = true;
        }
        if (any) p_.requestDisplayUpdate();
    }

private:
    mpc::Plugin& p_;
    std::mutex mtx_;
    std::vector<int> kind_, frames_, frame_, frameShown_, flashOf_, seen_, prev_;
    std::vector<std::string> texts_, msg_, shown_;
    std::vector<Clock::time_point> msgUntil_, flashUntil_;
};

} // namespace genraui

#endif
