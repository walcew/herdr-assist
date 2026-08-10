/**
 * @file
 * @brief Driver "pet": o mascote primitivo original (olhos, boca e bolinha).
 *
 * Mesmo desenho de antes, redimensionado para o slot em destaque da home.
 */

#include "avatar.h"
#include "ui_theme.h"

#define PET_SIZE 96

static lv_obj_t *s_eye[2];
static lv_obj_t *s_mouth;
static lv_obj_t *s_mood;

static void pet_create(lv_obj_t *parent)
{
    lv_obj_t *pet = ui_card(parent, 28);
    lv_obj_set_size(pet, PET_SIZE, PET_SIZE);
    lv_obj_set_style_border_width(pet, 1, 0);
    lv_obj_set_style_border_color(pet, UI_BORDER, 0);
    lv_obj_clear_flag(pet, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(pet);

    for (int i = 0; i < 2; i++) {
        s_eye[i] = lv_obj_create(pet);
        lv_obj_set_size(s_eye[i], 11, 17);
        lv_obj_set_style_radius(s_eye[i], 6, 0);
        lv_obj_set_style_bg_color(s_eye[i], UI_TEXT, 0);
        lv_obj_set_style_border_width(s_eye[i], 0, 0);
        lv_obj_align(s_eye[i], LV_ALIGN_TOP_MID, i == 0 ? -14 : 14, 29);
        lv_obj_clear_flag(s_eye[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(s_eye[i], LV_OBJ_FLAG_CLICKABLE);
    }

    s_mouth = lv_obj_create(pet);
    lv_obj_set_size(s_mouth, 19, 5);
    lv_obj_set_style_radius(s_mouth, 3, 0);
    lv_obj_set_style_bg_color(s_mouth, UI_MUTED, 0);
    lv_obj_set_style_border_width(s_mouth, 0, 0);
    lv_obj_align(s_mouth, LV_ALIGN_TOP_MID, 0, 57);
    lv_obj_clear_flag(s_mouth, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_mouth, LV_OBJ_FLAG_CLICKABLE);

    s_mood = lv_obj_create(pet);
    lv_obj_set_size(s_mood, 17, 17);
    lv_obj_set_style_radius(s_mood, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_mood, UI_IDLE, 0);
    lv_obj_set_style_border_width(s_mood, 3, 0);
    lv_obj_set_style_border_color(s_mood, UI_BG, 0);
    lv_obj_align(s_mood, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_clear_flag(s_mood, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_mood, LV_OBJ_FLAG_CLICKABLE);
}

static void pet_destroy(void)
{
    /* widgets morrem no lv_obj_clean do motor; só zerar os ponteiros */
    s_eye[0] = s_eye[1] = NULL;
    s_mouth = NULL;
    s_mood = NULL;
}

static void pet_set_state(avatar_state_t st)
{
    bool blocked = st == AVATAR_ST_BLOCKED;
    bool off     = st == AVATAR_ST_DISCONNECTED;

    /* humor: o pior estado manda; desconectado = cara "apagada" */
    lv_color_t mood = blocked ? UI_BLOCKED :
                      off     ? UI_MUTED :
                      st == AVATAR_ST_WORKING ? UI_WORKING :
                      st == AVATAR_ST_DONE    ? UI_DONE : UI_IDLE;
    lv_obj_set_style_bg_color(s_mood, mood, 0);

    lv_coord_t eye_h   = blocked ? 11 : off ? 8 : 17;   /* apertados = preocupado */
    lv_coord_t eye_y   = blocked ? 33 : off ? 34 : 29;
    lv_coord_t mouth_w = (blocked || off) ? 12 :
                         st == AVATAR_ST_WORKING ? 15 : 22;  /* larga = tudo bem */
    for (int i = 0; i < 2; i++) {
        lv_obj_set_height(s_eye[i], eye_h);
        lv_obj_align(s_eye[i], LV_ALIGN_TOP_MID, i == 0 ? -14 : 14, eye_y);
    }
    lv_obj_set_width(s_mouth, mouth_w);
}

const avatar_driver_t avatar_pet_driver = {
    .name      = "pet",
    .create    = pet_create,
    .destroy   = pet_destroy,
    .set_state = pet_set_state,
    .tick      = NULL,
};
