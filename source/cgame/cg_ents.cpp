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
#include "qcommon/cmodel.h"
#include "client/renderer/renderer.h"

static void CG_UpdateEntities();

static bool CG_UpdateLinearProjectilePosition( centity_t *cent ) {
	constexpr int MIN_DRAWDISTANCE_FIRSTPERSON = 86;
	constexpr int MIN_DRAWDISTANCE_THIRDPERSON = 52;

	SyncEntityState * state = &cent->current;

	if( !state->linearMovement ) {
		return false;
	}

	int64_t serverTime;
	if( GS_MatchPaused( &client_gs ) ) {
		serverTime = cg.frame.serverTime;
	} else {
		serverTime = cl.serverTime + cgs.extrapolationTime;
	}

	const cmodel_t * cmodel = CM_TryFindCModel( CM_Client, state->model );
	if( cmodel == NULL ) {
		// add a time offset to counter antilag visualization
		if( !cgs.demoPlaying && cg_projectileAntilagOffset->value > 0.0f &&
			!ISVIEWERENTITY( state->ownerNum ) && ( cgs.playerNum + 1 != cg.predictedPlayerState.POVnum ) ) {
			serverTime += state->linearMovementTimeDelta * cg_projectileAntilagOffset->value;
		}
	}

	Vec3 origin;
	int moveTime = GS_LinearMovement( state, serverTime, &origin );
	state->origin = origin;

	if( moveTime < 0 && cmodel == NULL ) {
		// when flyTime is negative don't offset it backwards more than PROJECTILE_PRESTEP value
		// FIXME: is this still valid?
		float maxBackOffset;

		if( ISVIEWERENTITY( state->ownerNum ) ) {
			maxBackOffset = PROJECTILE_PRESTEP - MIN_DRAWDISTANCE_FIRSTPERSON;
		} else {
			maxBackOffset = PROJECTILE_PRESTEP - MIN_DRAWDISTANCE_THIRDPERSON;
		}

		if( Length( state->origin - state->origin2 ) > maxBackOffset ) {
			return false;
		}
	}

	return true;
}

static void CG_NewPacketEntityState( SyncEntityState *state ) {
	centity_t * cent = &cg_entities[state->number];

	cent->prevVelocity = Vec3( 0.0f );
	cent->canExtrapolatePrev = false;

	if( ISEVENTENTITY( state ) ) {
		cent->prev = cent->current;
		cent->current = *state;
		cent->serverFrame = cg.frame.serverFrame;

		cent->velocity = Vec3( 0.0f );
		cent->canExtrapolate = false;
	} else if( state->linearMovement ) {
		if( cent->serverFrame != cg.oldFrame.serverFrame || state->teleported ||
			state->linearMovement != cent->current.linearMovement || state->linearMovementTimeStamp != cent->current.linearMovementTimeStamp ) {
			cent->prev = *state;
		} else {
			cent->prev = cent->current;
		}

		cent->current = *state;
		cent->serverFrame = cg.frame.serverFrame;

		cent->velocity = Vec3( 0.0f );
		cent->canExtrapolate = false;

		cent->linearProjectileCanDraw = CG_UpdateLinearProjectilePosition( cent );

		cent->velocity = cent->current.linearMovementVelocity;
		cent->trailOrigin = cent->current.origin;
	} else {
		// if it moved too much force the teleported bit
		if( Abs( cent->current.origin.x - state->origin.x ) > 512
			 || Abs( cent->current.origin.y - state->origin.y ) > 512
			 || Abs( cent->current.origin.z - state->origin.z ) > 512 ) {
			cent->serverFrame = -99;
		}

		// some data changes will force no lerping
		if( state->model != cent->current.model || state->teleported || state->linearMovement != cent->current.linearMovement ) {
			cent->serverFrame = -99;
		}

		if( cent->serverFrame != cg.oldFrame.serverFrame ) {
			// wasn't in last update, so initialize some things
			// duplicate the current state so lerping doesn't hurt anything
			cent->prev = *state;

			memset( cent->localEffects, 0, sizeof( cent->localEffects ) );

			// Init the animation when new into PVS
			if( cg.frame.valid && ( state->type == ET_PLAYER || state->type == ET_CORPSE ) ) {
				memset( cent->lastVelocities, 0, sizeof( cent->lastVelocities ) );
				memset( cent->lastVelocitiesFrames, 0, sizeof( cent->lastVelocitiesFrames ) );
				CG_PModel_ClearEventAnimations( state->number );
				memset( &cg_entPModels[state->number].animState, 0, sizeof( cg_entPModels[state->number].animState ) );
			}
		} else {   // shuffle the last state to previous
			cent->prev = cent->current;
		}

		if( cent->serverFrame != cg.oldFrame.serverFrame ) {
			cent->microSmooth = 0;
		}

		cent->current = *state;
		cent->trailOrigin = state->origin;
		cent->prevVelocity = cent->velocity;

		cent->canExtrapolatePrev = cent->canExtrapolate;
		cent->canExtrapolate = false;
		cent->velocity = Vec3( 0.0f );
		cent->serverFrame = cg.frame.serverFrame;

		// set up velocities for this entity
		if( cgs.extrapolationTime &&
			( cent->current.type == ET_PLAYER || cent->current.type == ET_CORPSE ) ) {
			cent->velocity = cent->current.origin2;
			cent->prevVelocity = cent->prev.origin2;
			cent->canExtrapolate = cent->canExtrapolatePrev = true;
		} else if( cent->prev.origin != cent->current.origin ) {
			float snapTime = ( cg.frame.serverTime - cg.oldFrame.serverTime );

			if( !snapTime ) {
				snapTime = cgs.snapFrameTime;
			}

			cent->velocity = ( cent->current.origin - cent->prev.origin ) * 1000.0f / snapTime;
		}

		if( ( cent->current.type == ET_GENERIC || cent->current.type == ET_PLAYER
			  || cent->current.type == ET_GRENADE
			  || cent->current.type == ET_CORPSE ) ) {
			cent->canExtrapolate = true;
		}

		if( CM_IsBrushModel( CM_Client, cent->current.model ) ) { // disable extrapolation on movers
			cent->canExtrapolate = false;
		}
	}
}

int CG_LostMultiviewPOV() {
	int best = client_gs.maxclients;
	int index = -1;
	int fallback = -1;

	for( int i = 0; i < cg.frame.numplayers; i++ ) {
		int value = Abs( (int)cg.frame.playerStates[i].playerNum - (int)cg.multiviewPlayerNum );
		if( value == best && i > index ) {
			continue;
		}

		if( value < best ) {
			if( cg.frame.playerStates[i].pmove.pm_type == PM_SPECTATOR ) {
				fallback = i;
				continue;
			}

			best = value;
			index = i;
		}
	}

	return index > -1 ? index : fallback;
}

static void CG_SetFramePlayerState( snapshot_t *frame, int index ) {
	frame->playerState = frame->playerStates[index];
	if( cgs.demoPlaying || cg.frame.multipov ) {
		frame->playerState.pmove.pm_flags |= PMF_NO_PREDICTION;
		if( frame->playerState.pmove.pm_type != PM_SPECTATOR ) {
			frame->playerState.pmove.pm_type = PM_CHASECAM;
		}
	}
}

static void CG_UpdatePlayerState() {
	int index = 0;

	if( cg.frame.multipov ) {
		// find the playerState containing our current POV, then cycle playerStates
		index = -1;
		for( int i = 0; i < cg.frame.numplayers; i++ ) {
			if( cg.frame.playerStates[i].playerNum < (unsigned)client_gs.maxclients
				&& cg.frame.playerStates[i].playerNum == cg.multiviewPlayerNum ) {
				index = i;
				break;
			}
		}

		// the POV was lost, find the closer one (may go up or down, but who cares)
		if( index < 0 || cg.frame.playerStates[index].pmove.pm_type == PM_SPECTATOR ) {
			index = CG_LostMultiviewPOV();
		}
		if( index < 0 ) {
			index = 0;
		}
	}

	cg.multiviewPlayerNum = cg.frame.playerStates[index].playerNum;

	// set up the playerstates

	// current
	CG_SetFramePlayerState( &cg.frame, index );

	// old
	index = -1;
	for( int i = 0; i < cg.oldFrame.numplayers; i++ ) {
		if( cg.oldFrame.playerStates[i].playerNum == cg.multiviewPlayerNum ) {
			index = i;
			break;
		}
	}

	// use the current one for old frame too, if correct POV wasn't found
	if( index == -1 ) {
		cg.oldFrame.playerState = cg.frame.playerState;
	} else {
		CG_SetFramePlayerState( &cg.oldFrame, index );
	}

	cg.predictedPlayerState = cg.frame.playerState;
}

/*
* CG_NewFrameSnap
* a new frame snap has been received from the server
*/
bool CG_NewFrameSnap( snapshot_t *frame, snapshot_t *lerpframe ) {
	assert( frame );

	if( lerpframe ) {
		cg.oldFrame = *lerpframe;
	} else {
		cg.oldFrame = *frame;
	}

	cg.frame = *frame;
	client_gs.gameState = frame->gameState;

	if( cg_projectileAntilagOffset->value > 1.0f || cg_projectileAntilagOffset->value < 0.0f ) {
		Cvar_ForceSet( "cg_projectileAntilagOffset", cg_projectileAntilagOffset->dvalue );
	}

	CG_UpdatePlayerState();

	for( int i = 0; i < frame->numEntities; i++ ) {
		CG_NewPacketEntityState( &frame->parsedEntities[i & ( MAX_PARSE_ENTITIES - 1 )] );
	}

	if( !cgs.precacheDone || !cg.frame.valid ) {
		return false;
	}

	// a new server frame begins now
	CG_BuildSolidList();
	ResetAnnouncerSpeakers();
	CG_UpdateEntities();
	CG_CheckPredictionError();

	cg.predictFrom = 0; // force the prediction to be restarted from the new snapshot
	cg.fireEvents = true;

	for( int i = 0; i < cg.frame.numgamecommands; i++ ) {
		int target = cg.frame.playerState.POVnum - 1;
		if( cg.frame.gamecommands[i].all || cg.frame.gamecommands[i].targets[target >> 3] & ( 1 << ( target & 7 ) ) ) {
			CG_GameCommand( cg.frame.gamecommandsData + cg.frame.gamecommands[i].commandOffset );
		}
	}

	CG_FireEvents( true );

	cg.firstFrame = false; // not the first frame anymore
	return true;
}

/*
* CG_CModelForEntity
*  get the collision model for the given entity, no matter if box or brush-model.
*/
const cmodel_t *CG_CModelForEntity( int entNum ) {
	if( entNum < 0 || entNum >= MAX_EDICTS ) {
		return NULL;
	}

	const centity_t * cent = &cg_entities[entNum];
	if( cent->serverFrame != cg.frame.serverFrame ) { // not present in current frame
		return NULL;
	}

	const cmodel_t * cmodel = CM_TryFindCModel( CM_Client, cent->current.model );
	if( cmodel != NULL )
		return cmodel;

	if( cent->type == ET_PLAYER || cent->type == ET_CORPSE ) {
		return CM_OctagonModelForBBox( cl.cms, cent->current.bounds.mins, cent->current.bounds.maxs );
	}

	return CM_ModelForBBox( cl.cms, cent->current.bounds.mins, cent->current.bounds.maxs );
}

static void CG_UpdateGenericEnt( centity_t *cent ) {
	// start from clean
	memset( &cent->interpolated, 0, sizeof( cent->interpolated ) );
	cent->interpolated.scale = 1.0f;
	cent->interpolated.color = RGBA8( CG_TeamColor( cent->current.team ) );

	// set up the model
	cent->interpolated.model = FindModel( cent->current.model );
}

void CG_ExtrapolateLinearProjectile( centity_t *cent ) {
	cent->linearProjectileCanDraw = CG_UpdateLinearProjectilePosition( cent );

	cent->interpolated.origin = cent->current.origin;
	cent->interpolated.origin2 = cent->current.origin;

	AnglesToAxis( cent->current.angles, cent->interpolated.axis );
}

void CG_LerpGenericEnt( centity_t *cent ) {
	Vec3 ent_angles = Vec3( 0, 0, 0 );

	if( ISVIEWERENTITY( cent->current.number ) || cg.view.POVent == cent->current.number ) {
		ent_angles = cg.predictedPlayerState.viewangles;
	} else {
		// interpolate angles
		ent_angles = LerpAngles( cent->prev.angles, cg.lerpfrac, cent->current.angles );
	}

	if( ent_angles.x || ent_angles.y || ent_angles.z ) {
		AnglesToAxis( ent_angles, cent->interpolated.axis );
	} else {
		Matrix3_Copy( axis_identity, cent->interpolated.axis );
	}

	if( ISVIEWERENTITY( cent->current.number ) || cg.view.POVent == cent->current.number ) {
		cent->interpolated.origin = cg.predictedPlayerState.pmove.origin;
		cent->interpolated.origin2 = cent->interpolated.origin;
	} else {
		if( cgs.extrapolationTime && cent->canExtrapolate ) { // extrapolation
			Vec3 origin, xorigin1, xorigin2;

			float lerpfrac = Clamp01( cg.lerpfrac );

			// extrapolation with half-snapshot smoothing
			if( cg.xerpTime >= 0 || !cent->canExtrapolatePrev ) {
				xorigin1 = cent->current.origin + cent->velocity * cg.xerpTime;
			} else {
				xorigin1 = cent->current.origin + cent->velocity * cg.xerpTime;
				if( cent->canExtrapolatePrev ) {
					Vec3 oldPosition = cent->prev.origin + cent->prevVelocity * cg.oldXerpTime;
					xorigin1 = Lerp( oldPosition, cg.xerpSmoothFrac, xorigin1 );
				}
			}


			// extrapolation with full-snapshot smoothing
			xorigin2 = cent->current.origin + cent->velocity * cg.xerpTime;
			if( cent->canExtrapolatePrev ) {
				Vec3 oldPosition = cent->prev.origin + cent->prevVelocity * cg.oldXerpTime;
				xorigin2 = Lerp( oldPosition, lerpfrac, xorigin2 );
			}

			origin = Lerp( xorigin1, 0.5f, xorigin2 );

			if( cent->microSmooth == 2 ) {
				Vec3 oldsmoothorigin = Lerp( cent->microSmoothOrigin2, 0.65f, cent->microSmoothOrigin );
				cent->interpolated.origin = Lerp( origin, 0.5f, oldsmoothorigin );
			} else if( cent->microSmooth == 1 ) {
				cent->interpolated.origin = Lerp( origin, 0.5f, cent->microSmoothOrigin );
			} else {
				cent->interpolated.origin = origin;
			}

			if( cent->microSmooth ) {
				cent->microSmoothOrigin2 = Vec3( cent->microSmoothOrigin );
			}

			cent->microSmoothOrigin = origin;
			cent->microSmooth = Min2( 2, cent->microSmooth + 1 );

			cent->interpolated.origin2 = cent->interpolated.origin;
		} else {   // plain interpolation
			cent->interpolated.origin = Lerp( cent->prev.origin, cg.lerpfrac, cent->current.origin );
			cent->interpolated.origin2 = cent->interpolated.origin;
		}
	}

	cent->interpolated.animating = cent->current.animating;
	cent->interpolated.animation_time = Lerp( cent->prev.animation_time, cg.lerpfrac, cent->current.animation_time );
}

static void DrawEntityModel( centity_t * cent ) {
	if( cent->interpolated.scale == 0.0f ) {
		return;
	}

	if( cent->interpolated.model == NULL ) {
		return;
	}

	TempAllocator temp = cls.frame_arena.temp();

	const Model * model = cent->interpolated.model;
	Mat4 transform = FromAxisAndOrigin( cent->interpolated.axis, cent->interpolated.origin );

	Vec4 color = sRGBToLinear( cent->interpolated.color );

	MatrixPalettes palettes = { };
	if( cent->interpolated.animating ) {
		Span< TRS > pose = SampleAnimation( &temp, model, cent->interpolated.animation_time );
		palettes = ComputeMatrixPalettes( &temp, model, pose );
	}

	DrawModel( model, transform, color, palettes );
	DrawModelShadow( model, transform, color, palettes );

	if( cent->current.silhouetteColor.a > 0 ) {
		if( ( cent->current.effects & EF_TEAM_SILHOUETTE ) == 0 || ISREALSPECTATOR() || cent->current.team == cg.predictedPlayerState.team ) {
			Vec4 silhouette_color = sRGBToLinear( cent->current.silhouetteColor );
			DrawModelSilhouette( model, transform, silhouette_color, palettes );
		}
	}

	if( cent->effects & EF_WORLD_MODEL ) {
		UniformBlock model_uniforms = UploadModelUniforms( transform * model->transform );
		for( u32 i = 0; i < model->num_primitives; i++ ) {
			if( model->primitives[ i ].material->blend_func == BlendFunc_Disabled ) {
				{
					PipelineState pipeline = MaterialToPipelineState( model->primitives[ i ].material );
					pipeline.set_uniform( "u_View", frame_static.view_uniforms );
					pipeline.set_uniform( "u_Model", model_uniforms );

					DrawModelPrimitive( model, &model->primitives[ i ], pipeline );
				}
				{
					PipelineState pipeline;
					pipeline.pass = frame_static.near_shadowmap_pass;
					pipeline.shader = &shaders.depth_only;
					pipeline.clamp_depth = true;
					pipeline.cull_face = CullFace_Disabled;
					pipeline.set_uniform( "u_View", frame_static.near_shadowmap_view_uniforms );
					pipeline.set_uniform( "u_Model", model_uniforms );

					DrawModelPrimitive( model, &model->primitives[ i ], pipeline );
				}
				{
					PipelineState pipeline;
					pipeline.pass = frame_static.far_shadowmap_pass;
					pipeline.shader = &shaders.depth_only;
					pipeline.clamp_depth = true;
					pipeline.cull_face = CullFace_Disabled;
					pipeline.set_uniform( "u_View", frame_static.far_shadowmap_view_uniforms );
					pipeline.set_uniform( "u_Model", model_uniforms );

					DrawModelPrimitive( model, &model->primitives[ i ], pipeline );
				}
			}
		}
	}
}

static void CG_AddPlayerEnt( centity_t *cent ) {
	if( ISVIEWERENTITY( cent->current.number ) ) {
		cg.effects = cent->effects;
	}

	// if set to invisible, skip
	if( cent->current.team == TEAM_SPECTATOR ) { // TODO remove?
		return;
	}

	CG_DrawPlayer( cent );
}

static void CG_LerpLaser( centity_t *cent ) {
	cent->interpolated.origin = Lerp( cent->prev.origin, cg.lerpfrac, cent->current.origin );
	cent->interpolated.origin2 = Lerp( cent->prev.origin2, cg.lerpfrac, cent->current.origin2 );
}

static void CG_AddLaserEnt( centity_t *cent ) {
	DrawBeam( cent->interpolated.origin, cent->interpolated.origin2, cent->current.radius, vec4_white, cgs.media.shaderLaser );
}

static void CG_UpdateLaserbeamEnt( centity_t *cent ) {
	centity_t *owner;

	if( cg.view.playerPrediction && ISVIEWERENTITY( cent->current.ownerNum ) ) {
		return;
	}

	owner = &cg_entities[cent->current.ownerNum];
	if( owner->serverFrame != cg.frame.serverFrame ) {
		Com_Error( ERR_DROP, "CG_UpdateLaserbeamEnt: owner is not in the snapshot\n" );
	}

	owner->localEffects[LOCALEFFECT_LASERBEAM] = cl.serverTime + 10;

	// laser->s.origin is beam start
	// laser->s.origin2 is beam end

	owner->laserOriginOld = cent->prev.origin;
	owner->laserPointOld = cent->prev.origin2;

	owner->laserOrigin = cent->current.origin;
	owner->laserPoint = cent->current.origin2;
}

static void CG_LerpLaserbeamEnt( centity_t *cent ) {
	centity_t *owner = &cg_entities[cent->current.ownerNum];

	if( cg.view.playerPrediction && ISVIEWERENTITY( cent->current.ownerNum ) ) {
		return;
	}

	owner->localEffects[LOCALEFFECT_LASERBEAM] = cl.serverTime + 1;
}

void CG_SoundEntityNewState( centity_t *cent ) {
	int owner = cent->current.ownerNum;
	int channel = cent->current.channel & ~CHAN_FIXED;
	bool fixed = ( cent->current.channel & CHAN_FIXED ) != 0;

	if( cent->current.svflags & SVF_BROADCAST ) {
		S_StartGlobalSound( cent->current.sound, channel, 1.0f );
		return;
	}

	if( owner ) {
		if( owner < 0 || owner >= MAX_EDICTS ) {
			Com_Printf( "CG_SoundEntityNewState: bad owner number" );
			return;
		}
		if( cg_entities[owner].serverFrame != cg.frame.serverFrame ) {
			owner = 0;
		}
	}

	if( !owner ) {
		fixed = true;
	}

	if( fixed ) {
		S_StartFixedSound( cent->current.sound, cent->current.origin, channel, 1.0f );
	}
	else if( ISVIEWERENTITY( owner ) ) {
		S_StartGlobalSound( cent->current.sound, channel, 1.0f );
	}
	else {
		S_StartEntitySound( cent->current.sound, owner, channel, 1.0f );
	}
}

static void CG_LerpSpikes( centity_t * cent ) {
	constexpr float retracted = -48;
	constexpr float primed = -36;
	constexpr float extended = 0;

	float position = retracted;

	if( cent->current.radius == 1 ) {
		position = extended;
	}
	else if( cent->current.linearMovementTimeStamp != 0 ) {
		int64_t delta = Lerp( cg.oldFrame.serverTime, cg.lerpfrac, cg.frame.serverTime ) - cent->current.linearMovementTimeStamp;
		if( delta > 0 ) {
			// 0-100: jump to primed
			// 1000-1050: fully extend
			// 1500-2000: retract
			if( delta < 1000 ) {
				float t = Min2( 1.0f, Unlerp( int64_t( 0 ), delta, int64_t( 100 ) ) );
				position = Lerp( retracted, t, primed );
			}
			else if( delta < 1050 ) {
				float t = Min2( 1.0f, Unlerp( int64_t( 1000 ), delta, int64_t( 1050 ) ) );
				position = Lerp( primed, t, extended );
			}
			else {
				float t = Max2( 0.0f, Unlerp( int64_t( 1500 ), delta, int64_t( 2000 ) ) );
				position = Lerp( extended, t, retracted );
			}
		}
	}

	Vec3 up;
	AngleVectors( cent->current.angles, NULL, NULL, &up );

	AnglesToAxis( cent->current.angles, cent->interpolated.axis );
	cent->interpolated.origin = cent->current.origin + up * position;
	cent->interpolated.origin2 = cent->interpolated.origin;
}

static void CG_UpdateSpikes( centity_t * cent ) {
	CG_UpdateGenericEnt( cent );

	if( cent->current.linearMovementTimeStamp == 0 )
		return;

	int64_t old_delta = cg.oldFrame.serverTime - cent->current.linearMovementTimeStamp;
	int64_t delta = cg.frame.serverTime - cent->current.linearMovementTimeStamp;

	if( old_delta < 0 && delta >= 0 ) {
		S_StartEntitySound( "sounds/spikes/arm", cent->current.number, CHAN_AUTO, 1.0f );
	}
	else if( old_delta < 1000 && delta >= 1000 ) {
		S_StartEntitySound( "sounds/spikes/retract", cent->current.number, CHAN_AUTO, 1.0f );
	}
	else if( old_delta < 1050 && delta >= 1050 ) {
		S_StartEntitySound( "sounds/spikes/glint", cent->current.number, CHAN_AUTO, 1.0f );
	}
	else if( old_delta < 1500 && delta >= 1500 ) {
		S_StartEntitySound( "sounds/spikes/retract", cent->current.number, CHAN_AUTO, 1.0f );
	}
}

void CG_EntityLoopSound( centity_t * cent, SyncEntityState * state ) {
	cent->sound = S_ImmediateEntitySound( state->sound, state->number, 1.0f, cent->sound );
}

static void DrawEntityTrail( const centity_t * cent, StringHash name ) {
	// didn't move
	Vec3 vec = cent->interpolated.origin - cent->trailOrigin;
	float len = Length( vec );
	if( len == 0 )
		return;

	Vec4 color = Vec4( CG_TeamColorVec4( cent->current.team ).xyz(), 0.5f );
	DoVisualEffect( name, cent->interpolated.origin, cent->trailOrigin, 1.0f, color );
}

void DrawEntities() {
	ZoneScoped;

	for( int pnum = 0; pnum < cg.frame.numEntities; pnum++ ) {
		SyncEntityState * state = &cg.frame.parsedEntities[pnum & ( MAX_PARSE_ENTITIES - 1 )];
		centity_t * cent = &cg_entities[state->number];

		if( cent->current.linearMovement ) {
			if( !cent->linearProjectileCanDraw ) {
				continue;
			}
		}

		switch( cent->type ) {
			case ET_GENERIC:
				DrawEntityModel( cent );
				CG_EntityLoopSound( cent, state );
				break;

			case ET_ROCKET:
				DrawEntityModel( cent );
				DrawEntityTrail( cent, "weapons/rl/trail" );
				DrawDynamicLight( cent->interpolated.origin, CG_TeamColorVec4( cent->current.team ), 25600.0f );
				CG_EntityLoopSound( cent, state );
				break;
			case ET_GRENADE:
				DrawEntityModel( cent );
				DrawEntityTrail( cent, "weapons/gl/trail" );
				DrawDynamicLight( cent->interpolated.origin, CG_TeamColorVec4( cent->current.team ), 6400.0f );
				CG_EntityLoopSound( cent, state );
				break;
			case ET_ARBULLET:
				DrawEntityModel( cent );
				DrawEntityTrail( cent, "weapons/ar/trail" );
				DrawDynamicLight( cent->interpolated.origin, CG_TeamColorVec4( cent->current.team ), 6400.0f );
				CG_EntityLoopSound( cent, state );
				break;
			case ET_BUBBLE:
				DrawEntityModel( cent );
				DrawEntityTrail( cent, "weapons/bg/trail" );
				DrawDynamicLight( cent->interpolated.origin, CG_TeamColorVec4( cent->current.team ), 6400.0f );
				CG_EntityLoopSound( cent, state );
				break;
			case ET_RIFLEBULLET:
				DrawEntityModel( cent );
				DrawEntityTrail( cent, "weapons/rifle/bullet_trail" );
				CG_EntityLoopSound( cent, state );
				break;
			case ET_STAKE:
				DrawEntityModel( cent );
				DrawEntityTrail( cent, "weapons/stake/trail" );
				CG_EntityLoopSound( cent, state );
				break;
			case ET_BLAST:
				DrawEntityTrail( cent, "weapons/mb/trail" );
				DrawDynamicLight( cent->interpolated.origin, CG_TeamColorVec4( cent->current.team ), 3200.0f );
				CG_EntityLoopSound( cent, state );
				break;

			case ET_PLAYER:
				CG_AddPlayerEnt( cent );
				CG_EntityLoopSound( cent, state );
				CG_LaserBeamEffect( cent );
				break;

			case ET_CORPSE:
				CG_AddPlayerEnt( cent );
				CG_EntityLoopSound( cent, state );
				break;

			case ET_GHOST:
				break;

			case ET_DECAL: {
				Vec3 normal;
				AngleVectors( cent->current.angles, &normal, NULL, NULL );
				DrawDecal( cent->current.origin, normal, cent->current.radius, cent->current.angles.z, cent->current.material, sRGBToLinear( cent->current.color ) );
			} break;

			case ET_LASERBEAM:
				break;

			case ET_JUMPPAD:
			case ET_PAINKILLER_JUMPPAD:
				CG_EntityLoopSound( cent, state );
				break;

			case ET_EVENT:
			case ET_SOUNDEVENT:
				break;

			case ET_BOMB:
				CG_AddBomb( cent );
				break;

			case ET_BOMB_SITE:
				CG_AddBombSite( cent );
				break;

			case ET_LASER:
				CG_AddLaserEnt( cent );
				cent->sound = S_ImmediateLineSound( state->sound, cent->interpolated.origin, cent->interpolated.origin2, 1.0f, cent->sound );

			case ET_SPIKES:
				DrawEntityModel( cent );
				break;

			case ET_SPEAKER:
				DrawEntityModel( cent );
				break;

			default:
				Com_Error( ERR_DROP, "DrawEntities: unknown entity type" );
				break;
		}

		cent->trailOrigin = cent->interpolated.origin;
	}
}

/*
* CG_LerpEntities
* Interpolate the entity states positions into the entity_t structs
*/
void CG_LerpEntities() {
	ZoneScoped;

	for( int pnum = 0; pnum < cg.frame.numEntities; pnum++ ) {
		SyncEntityState * state = &cg.frame.parsedEntities[pnum & ( MAX_PARSE_ENTITIES - 1 )];
		int number = state->number;
		centity_t * cent = &cg_entities[number];

		switch( cent->type ) {
			case ET_GENERIC:
			case ET_ROCKET:
			case ET_ARBULLET:
			case ET_BUBBLE:
			case ET_GRENADE:
			case ET_RIFLEBULLET:
			case ET_STAKE:
			case ET_BLAST:
			case ET_PLAYER:
			case ET_CORPSE:
			case ET_GHOST:
			case ET_SPEAKER:
			case ET_BOMB:
				if( state->linearMovement ) {
					CG_ExtrapolateLinearProjectile( cent );
				} else {
					CG_LerpGenericEnt( cent );
				}
				break;

			case ET_DECAL:
				break;

			case ET_LASERBEAM:
				CG_LerpLaserbeamEnt( cent );
				break;

			case ET_JUMPPAD:
			case ET_PAINKILLER_JUMPPAD:
				break;

			case ET_EVENT:
			case ET_SOUNDEVENT:
				break;

			case ET_BOMB_SITE:
				break;

			case ET_LASER:
				CG_LerpLaser( cent );
				break;

			case ET_SPIKES:
				CG_LerpSpikes( cent );
				break;

			default:
				Com_Error( ERR_DROP, "CG_LerpEntities: unknown entity type" );
				break;
		}

		Vec3 origin, velocity;
		CG_GetEntitySpatialization( number, &origin, &velocity );
		S_UpdateEntity( number, origin, velocity );
	}
}

/*
* CG_UpdateEntities
* Called at receiving a new serverframe. Sets up the model, type, etc to be drawn later on
*/
void CG_UpdateEntities() {
	ZoneScoped;

	for( int pnum = 0; pnum < cg.frame.numEntities; pnum++ ) {
		SyncEntityState * state = &cg.frame.parsedEntities[pnum & ( MAX_PARSE_ENTITIES - 1 )];

		if( cgs.demoPlaying ) {
			if( ( state->svflags & SVF_ONLYTEAM ) && cg.predictedPlayerState.team != state->team )
				continue;
			if( ( ( state->svflags & SVF_ONLYOWNER ) || ( state->svflags & SVF_OWNERANDCHASERS ) ) && checked_cast< int >( cg.predictedPlayerState.POVnum ) != state->ownerNum )
				continue;
		}

		centity_t * cent = &cg_entities[state->number];
		cent->type = state->type;
		cent->effects = state->effects;

		switch( cent->type ) {
			case ET_GENERIC:
			case ET_ROCKET:
			case ET_ARBULLET:
			case ET_BUBBLE:
			case ET_GRENADE:
			case ET_RIFLEBULLET:
			case ET_STAKE:
			case ET_BLAST:
				CG_UpdateGenericEnt( cent );
				break;

			case ET_PLAYER:
			case ET_CORPSE:
				CG_UpdatePlayerModelEnt( cent );
				break;

			case ET_GHOST:
				break;

			case ET_DECAL:
				break;

			case ET_LASERBEAM:
				CG_UpdateLaserbeamEnt( cent );
				break;

			case ET_JUMPPAD:
			case ET_PAINKILLER_JUMPPAD:
				break;

			case ET_EVENT:
			case ET_SOUNDEVENT:
				break;

			case ET_BOMB:
			case ET_BOMB_SITE:
				break;

			case ET_LASER:
				break;

			case ET_SPIKES:
				CG_UpdateSpikes( cent );
				break;

			case ET_SPEAKER:
				CG_UpdateGenericEnt( cent );
				AddAnnouncerSpeaker( cent );
				break;

			default:
				Com_Error( ERR_DROP, "CG_UpdateEntities: unknown entity type %i", cent->type );
				break;
		}
	}
}

void CG_GetEntitySpatialization( int entNum, Vec3 * origin, Vec3 * velocity ) {
	const centity_t * cent = &cg_entities[ entNum ];

	if( velocity != NULL ) {
		*velocity = cent->velocity;
	}

	const cmodel_t * cmodel = CM_TryFindCModel( CM_Client, cent->current.model );
	if( cmodel == NULL ) {
		if( origin != NULL ) {
			*origin = cent->interpolated.origin;
		}
	}
	else {
		Vec3 mins, maxs;
		CM_InlineModelBounds( cl.cms, cmodel, &mins, &maxs );
		*origin = maxs + mins;
		*origin = cent->interpolated.origin + *origin * 0.5f;
	}
}
