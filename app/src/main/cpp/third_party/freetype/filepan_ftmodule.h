// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// FreeType's module list for FilePan, used in place of freetype/config/ftmodule.h through
// FT_CONFIG_MODULES_H. Only what FilePan's fonts and Android's system fonts need: TrueType and
// CFF outlines in SFNT containers, drawn by the smooth (anti-aliased) rasterizer. FilePan renders
// unhinted, so the auto-hinter is left out, and the static linker drops the drivers nothing
// registers (Type 1, PCF, BDF, PFR, SDF, SVG...).

FT_USE_MODULE( FT_Driver_ClassRec, tt_driver_class )
FT_USE_MODULE( FT_Driver_ClassRec, cff_driver_class )
FT_USE_MODULE( FT_Module_Class, psaux_module_class )
FT_USE_MODULE( FT_Module_Class, psnames_module_class )
FT_USE_MODULE( FT_Module_Class, pshinter_module_class )
FT_USE_MODULE( FT_Module_Class, sfnt_module_class )
FT_USE_MODULE( FT_Renderer_Class, ft_smooth_renderer_class )
