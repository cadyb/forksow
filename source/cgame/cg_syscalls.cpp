/*
Copyright (C) 2002-2003 Victor Luchits

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#include "cgame/cg_local.h"

cgame_import_t CGAME_IMPORT;

/*
* GetCGameAPI
*
* Returns a pointer to the structure with all entry points
*/
cgame_export_t * GetCGameAPI( cgame_import_t * import ) {
	static cgame_export_t globals;

	CGAME_IMPORT = *import;

	globals.Init = CG_Init;
	globals.Reset = CG_Reset;
	globals.Shutdown = CG_Shutdown;

	globals.ConfigString = CG_ConfigString;

	globals.EscapeKey = CG_EscapeKey;

	globals.Trace = CG_Trace;
	globals.RenderView = CG_RenderView;

	globals.NewFrameSnapshot = CG_NewFrameSnap;

	globals.GetButtonBits = CG_GetButtonBits;
	globals.MouseMove = CG_MouseMove;

	return &globals;
}
