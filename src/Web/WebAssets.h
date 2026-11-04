/**
 * expLORA Gateway
 *
 * The stylesheet and script shared by every page, served once as /a.css
 * and /a.js (public, cached for a year). Pages link them with ?v=TOKEN, a
 * hash of both texts, so a firmware with other assets is never shown with
 * a cached old copy
 *
 * Copyright Pajenicko s.r.o., Igor Sverma (C) 2025
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <cstdint>

namespace WebAssets
{
// Colours as variables (light and dark scheme); .ok/.w/.e state colours,
// .m muted, .b button link, .b2 secondary, .x destructive, .g field grid,
// .kv key-value table, .hm hidden on phones, form.i inline form; the nav
// wraps its entries onto more lines when the width runs out (down to 320 px)
inline constexpr char CSS[] =
    ":root{--bg:#f4f5f7;--fg:#1d2329;--m:#69727d;--c:#fff;--l:#dde1e6;--a:#0b6bcb;--ok:#1e8e3e;--w:#b26a00;"
    "--e:#c5221f}\n"
    "@media(prefers-color-scheme:dark){:root{--bg:#15181c;--fg:#e4e7eb;--m:#9aa3ad;--c:#1f2328;--l:#30363d;"
    "--a:#4c9be8;--ok:#3fb950;--w:#d29922;--e:#f85149}}\n"
    "*{box-sizing:border-box}\n"
    "body{margin:0;font:15px/1.45 system-ui,sans-serif;background:var(--bg);color:var(--fg)}\n"
    "header{display:flex;flex-wrap:wrap;align-items:baseline;gap:.2rem 1rem;padding:.6rem 1rem;"
    "background:var(--c);border-bottom:1px solid var(--l)}\n"
    "header b{margin-right:auto}\n"
    "nav{display:flex;flex-wrap:wrap;gap:.2rem .7rem}\n"
    "nav a{color:var(--m);text-decoration:none;white-space:nowrap}\n"
    "nav a.on{color:var(--a);font-weight:600}\n"
    "a{color:var(--a)}\n"
    "main{max-width:52rem;margin:auto;padding:1rem}\n"
    "section{background:var(--c);border:1px solid var(--l);border-radius:8px;padding:1rem;margin-bottom:1rem;"
    "overflow-x:auto}\n"
    "h2{font-size:1.05rem;margin:0 0 .6rem}\n"
    "h3{font-size:.95rem;margin:1rem 0 .3rem}\n"
    "p{margin:.4rem 0}\n"
    "table{width:100%;border-collapse:collapse}\n"
    "th,td{text-align:left;padding:.4rem .5rem;border-bottom:1px solid var(--l);vertical-align:top}\n"
    "th{color:var(--m);font-weight:600;font-size:.85rem}\n"
    ".kv td:first-child{color:var(--m);width:40%}\n"
    "label{display:block;margin:.7rem 0 .2rem;font-weight:600;font-size:.9rem}\n"
    "label.c{display:flex;gap:.5rem;align-items:center;font-weight:400}\n"
    "input,select{width:100%;padding:.45rem .55rem;border:1px solid var(--l);border-radius:6px;"
    "background:var(--bg);color:inherit;font:inherit}\n"
    "input[type=checkbox],input[type=radio],input[type=file]{width:auto}\n"
    "button,.b{display:inline-block;margin:.8rem .4rem 0 0;padding:.45rem .9rem;border:1px solid var(--a);"
    "border-radius:6px;background:var(--a);color:#fff;font:inherit;text-decoration:none;cursor:pointer}\n"
    ".b2{background:none;color:var(--a);border-color:var(--l)}\n"
    ".x{background:var(--e);border-color:var(--e)}\n"
    "td .b,td button{margin:0;padding:.2rem .6rem}\n"
    "form.i{display:inline}\n"
    "small,.m{color:var(--m);font-size:.85rem}\n"
    ".ok{color:var(--ok)}.w{color:var(--w)}.e{color:var(--e)}\n"
    ".g{display:grid;grid-template-columns:repeat(auto-fill,minmax(10rem,1fr));gap:0 1rem}\n"
    "code,pre{font:.85rem ui-monospace,monospace}\n"
    "pre{overflow-x:auto}\n"
    ".log-entry{font:.8rem ui-monospace,monospace;white-space:pre-wrap;border-bottom:1px solid var(--l);"
    "padding:.15rem 0}\n"
    ".log-error{color:var(--e)}.log-warning{color:var(--w)}.log-debug,.log-verbose{color:var(--m)}\n"
    "[hidden]{display:none!important}\n"
    "footer{text-align:center;color:var(--m);font-size:.8rem;padding:0 1rem 1rem}\n"
    "@media(max-width:600px){.hm{display:none}main{padding:.6rem}}\n";

// data-if='id' shows the element while checkbox id is checked,
// data-if='id=value' while select id has that value; the inputs of a
// hidden element are disabled, so they are not posted (the handlers keep
// the stored values of fields that are absent); applied again on pageshow,
// after the browser restored a form (back / forward cache, soft reload)
inline constexpr char JS[] =
    "function u(){document.querySelectorAll('[data-if]').forEach(function(e){var p=e.dataset.if.split('='),"
    "c=document.getElementById(p[0]);e.hidden=p[1]?c.value!=p[1]:!c.checked;"
    "e.querySelectorAll('input,select').forEach(function(i){i.disabled=e.hidden})})}"
    "document.addEventListener('change',u);addEventListener('pageshow',u);u();\n";

constexpr uint32_t fnv1a(const char *s, uint32_t h = 2166136261u)
{
    while (*s)
    {
        h = (h ^ (uint8_t)*s++) * 16777619u;
    }
    return h;
}

// Version token of both assets (the ?v= of their URLs)
constexpr uint32_t TOKEN = fnv1a(JS, fnv1a(CSS));
} // namespace WebAssets
