/**
 * expLORA protocol generation 2 - umbrella header
 *
 * Normative description: docs/protocol-v2.md (gateway repository).
 * This library is kept identical in expLORA-Gateway and expLORA-Sensors-gen2:
 * edit it in the gateway repository and run scripts/sync_protocol.sh.
 *
 * Copyright Pajenicko s.r.o., Igor Sverma (C) 2026
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include "ep_bytes.h"
#include "ep_counter.h"
#include "ep_crypto.h"
#include "ep_frame.h"
#include "ep_keys.h"
#include "ep_ota.h"
#include "ep_tlv.h"
