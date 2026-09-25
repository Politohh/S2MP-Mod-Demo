The ReShade API headers in this directory come from
https://github.com/crosire/reshade/tree/7bf9de8b33bcc76c3177007e65d73c72dd0f34c0/include
(API version 20). They are copyright Patrick Mours and carry the
BSD-3-Clause OR MIT SPDX notice. See LICENSE.md for the upstream license.

S2MP links no ReShade binary. At runtime it registers an optional
`reshade_finish_effects` callback if the installed ReShade build exposes a
compatible add-on API. If registration is unavailable, ordinary game-frame
capture remains operational without ReShade effects.
