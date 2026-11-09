# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 FilePan contributors

# Appends an #include of USER_CONFIG to CONFIG once.
file(READ "${CONFIG}" content)
set(line "#include \"${USER_CONFIG}\"")
string(FIND "${content}" "${line}" index)
if (index EQUAL -1)
    file(APPEND "${CONFIG}" "\n/* Added by FilePan. */\n${line}\n")
endif ()
