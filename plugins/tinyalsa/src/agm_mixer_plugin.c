/*
** Copyright (c) 2019, The Linux Foundation. All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions are
** met:
**   * Redistributions of source code must retain the above copyright
**     notice, this list of conditions and the following disclaimer.
**   * Redistributions in binary form must reproduce the above
**     copyright notice, this list of conditions and the following
**     disclaimer in the documentation and/or other materials provided
**     with the distribution.
**   * Neither the name of The Linux Foundation nor the names of its
**     contributors may be used to endorse or promote products derived
**     from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
** WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
** MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
** ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
** BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
** CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
** SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
** BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
** WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
** OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
** IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**/

/* agm_mixer.c all names (variable/functions) should have amp_ (Agm Mixer Plugin) */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include <cutils/list.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <limits.h>
#include <linux/ioctl.h>

#include <sound/asound.h>

#include <tinyalsa/mixer_plugin.h>
#include <tinyalsa/asoundlib.h>

#include <agm/agm_api.h>
#include <snd-card-def.h>

#define ARRAY_SIZE(a)    \
    (sizeof(a) / sizeof(a[0]))

#define AMP_PRIV_GET_CTL_PTR(p, idx) \
    (p->ctls + idx)

#define AMP_PRIV_GET_CTL_NAME_PTR(p, idx) \
    (p->ctl_names[idx])

#define BE_CTL_NAME_MEDIA_CONFIG 0
#define BE_CTL_NAME_METADATA 1

/* strings should be at the index as per the #defines */
static char *amp_be_ctl_name_extn[] = {
    "rate ch fmt",
    "metadata",
};

enum {
    PCM_CTL_NAME_CONNECT = 0,
    PCM_CTL_NAME_DISCONNECT,
    PCM_CTL_NAME_MTD_CONTROL,
    PCM_CTL_NAME_METADATA,
    PCM_CTL_NAME_SET_PARAM,
    PCM_CTL_NAME_SET_PARAM_TAG = 5,
    PCM_CTL_NAME_GET_TAG_INFO,
    PCM_CTL_NAME_EVENT,
    PCM_CTL_NAME_SET_CALIBRATION,
    PCM_CTL_NAME_GET_PARAM,
    /* Add new ones here */
};

/* strings should be at the index as per the #defines */
static char *amp_pcm_ctl_name_extn[] = {
    "connect",
    "disconnect",
    "control",
    "metadata",
    "setParam",
    "setParamTag",
    "getTaggedInfo",
    "event",
    "setCalibration",
    "getParam",
    /* Add new ones below, be sure to update enum as well */
};

enum {
    PCM_TX_CTL_NAME_LOOPBACK = 0,
    PCM_TX_CTL_NAME_ECHOREF,
    /* Add new ones here */
};
/* strings should be at the index as per the #defines */
static char *amp_pcm_tx_ctl_names[] = {
    "loopback",
    "echoReference"
    /* Add new ones here, be sue to update enum as well */
};

enum {
    PCM_RX_CTL_NAME_SIDETONE = 0,
};
/* strings should be at the index as per the enum */
static char *amp_pcm_rx_ctl_names[] = {
    "sidetone",
};

struct amp_dev_info {
    char **names;
    int *idx_arr;
    int count;
    struct snd_value_enum dev_enum;
    enum direction dir;

    /*
     * Mixer ctl data cache for
     * "pcm<id> metadata_control"
     * Unused for BE devs
     */
    int *pcm_mtd_ctl;

    /*
     * Mixer ctl data cache for
     * "pcm<id> getParam"
     * Unused for BE devs
     */
    void *get_param_payload;
    int get_param_payload_size;
};

struct amp_priv {
    unsigned int card;
    void *card_node;

    struct aif_info *aif_list;
    struct listnode events_list;
    struct listnode events_paramlist;

    struct amp_dev_info rx_be_devs;
    struct amp_dev_info tx_be_devs;
    struct amp_dev_info rx_pcm_devs;
    struct amp_dev_info tx_pcm_devs;

    struct snd_control *ctls;
    char (*ctl_names)[AIF_NAME_MAX_LEN + 16];
    int ctl_count;

    struct snd_value_enum tx_be_enum;
    struct snd_value_enum rx_be_enum;

    struct agm_media_config media_fmt;
    event_callback event_cb;
};

struct event_params_node {
    uint32_t session_id;
    struct listnode node;
    struct agm_event_cb_params event_params;
};

struct mixer_plugin_event_data {
    struct snd_ctl_event ev;
    struct listnode node;
};

static enum agm_media_format alsa_to_agm_fmt(int fmt)
{
    enum agm_media_format agm_pcm_fmt = AGM_FORMAT_INVALID;

    switch (fmt) {
    case SNDRV_PCM_FORMAT_S8:
        agm_pcm_fmt = AGM_FORMAT_PCM_S8;
        break;
    case SNDRV_PCM_FORMAT_S16_LE:
        agm_pcm_fmt = AGM_FORMAT_PCM_S16_LE;
        break;
    case SNDRV_PCM_FORMAT_S24_LE:
        agm_pcm_fmt = AGM_FORMAT_PCM_S24_LE;
        break;
    case SNDRV_PCM_FORMAT_S24_3LE:
        agm_pcm_fmt = AGM_FORMAT_PCM_S24_3LE;
        break;
    case SNDRV_PCM_FORMAT_S32_LE:
        agm_pcm_fmt = AGM_FORMAT_PCM_S32_LE;
        break;
    }

    return agm_pcm_fmt;
}

static struct amp_dev_info *amp_get_be_adi(struct amp_priv *amp_priv,
                enum direction dir)
{
    if (dir == RX)
        return &amp_priv->rx_be_devs;
    else if (dir == TX)
        return &amp_priv->tx_be_devs;

    return NULL;
}

static void amp_free_dev_info(struct amp_dev_info *adi)
{
    if (adi->names) {
        free(adi->names);
        adi->names = NULL;
    }

    if (adi->idx_arr) {
        free(adi->idx_arr);
        adi->idx_arr = NULL;
    }

    if (adi->pcm_mtd_ctl) {
        free(adi->pcm_mtd_ctl);
        adi->pcm_mtd_ctl = NULL;
    }

    if (adi->get_param_payload) {
        free(adi->get_param_payload);
        adi->get_param_payload = NULL;
    }

    adi->count = 0;
}

static void amp_free_be_dev_info(struct amp_priv *amp_priv)
{
    amp_free_dev_info(&amp_priv->rx_be_devs);
    amp_free_dev_info(&amp_priv->tx_be_devs);

    if (amp_priv->aif_list) {
        free(amp_priv->aif_list);
        amp_priv->aif_list = NULL;
    }
}

static void amp_free_pcm_dev_info(struct amp_priv *amp_priv)
{
    amp_free_dev_info(&amp_priv->rx_pcm_devs);
    amp_free_dev_info(&amp_priv->tx_pcm_devs);
}

static void amp_free_ctls(struct amp_priv *amp_priv)
{
    if (amp_priv->ctl_names) {
        free(amp_priv->ctl_names);
        amp_priv->ctl_names = NULL;
    }

    if (amp_priv->ctls) {
        free(amp_priv->ctls);
        amp_priv->ctls = NULL;
    }

    amp_priv->ctl_count = 0;
}

static void amp_add_event_params(struct amp_priv *amp_priv,
                                 uint32_t session_id,
                                 struct agm_event_cb_params *event_params)
{
    struct event_params_node *event_node;
    struct agm_event_cb_params *eparams;
    uint32_t len = event_params->event_payload_size;

    event_node = calloc(1, sizeof(struct event_params_node) + len);
    if (!event_node)
        return;

    event_node->session_id = session_id;
    eparams = &event_node->event_params;
    eparams->source_module_id = event_params->source_module_id;
    eparams->event_id = event_params->event_id;
    eparams->event_payload_size = len;

    memcpy(&eparams->event_payload, &event_params->event_payload, len);
    list_add_tail(&amp_priv->events_paramlist, &event_node->node);
}

void amp_event_cb(uint32_t session_id, struct agm_event_cb_params *event_params, void *client_data)
{
    struct mixer_plugin *plugin = client_data;
    struct amp_priv *amp_priv;
    struct snd_ctl_event event;
    struct mixer_plugin_event_data *data;
    char *stream = "PCM";
    char *ctl_name = "event";
    char *mixer_str = NULL;
    int ctl_len;

    if (!plugin)
        return;

    if (!plugin->subscribed)
        return;

    amp_priv = plugin->priv;
    if (!amp_priv)
        return;

    amp_add_event_params(amp_priv, session_id, event_params);

    /* To support Compress device related event instead of PCM
       session_id needs to go through adi list to check if it belongs
       to pcm device or compress device.
     */
    event.type = SNDRV_CTL_EVENT_ELEM;
    ctl_len = (int)(strlen(stream) + 4 + strlen(ctl_name) + 1);
    mixer_str = calloc(1, ctl_len);
    if (!mixer_str)
        return;

    snprintf(mixer_str, ctl_len, "%s%d %s", stream, session_id, ctl_name);
    strlcpy(event.data.elem.id.name, mixer_str, sizeof(event.data.elem.id.name));

    data = calloc(1, sizeof(struct mixer_plugin_event_data));
    if (!data)
        goto done;

    data->ev = event;
    list_add_tail(&amp_priv->events_list, &data->node);

    if (amp_priv->event_cb)
        amp_priv->event_cb(plugin);

done:
    free(mixer_str);
}

static void amp_copy_be_names_from_aif_list(struct aif_info *aif_list,
                size_t aif_cnt, struct amp_dev_info *adi, enum direction dir)
{
    struct aif_info *aif_info;
    int i, be_idx = 0;

    be_idx = 0;
    adi->names[be_idx] = "ZERO";
    adi->idx_arr[be_idx] = 0;
    be_idx++;

    for (i = 0; i < aif_cnt; i++) {
        aif_info = aif_list + i;
        if (aif_info->dir != dir)
            continue;

        adi->names[be_idx] = aif_info->aif_name;
        adi->idx_arr[be_idx] = i;
        be_idx++;
    }

    adi->count = be_idx;
    adi->dev_enum.items = adi->count;
    adi->dev_enum.texts = adi->names;
}

static int amp_get_be_info(struct amp_priv *amp_priv)
{
    struct amp_dev_info *rx_adi = &amp_priv->rx_be_devs;
    struct amp_dev_info *tx_adi = &amp_priv->tx_be_devs;
    struct aif_info *aif_list, *aif_info;
    size_t be_count = 0;
    int ret = 0, i;

    ret = agm_get_aif_info_list(NULL, &be_count);
    if (ret || be_count == 0)
        return -EINVAL;

    aif_list = calloc(be_count, sizeof(struct aif_info));
    if (!aif_list)
        return -ENOMEM;

    ret = agm_get_aif_info_list(aif_list, &be_count);
    if (ret)
        goto err_backends_get;

    rx_adi->count = 0;
    tx_adi->count = 0;

    /* count rx and tx backends */
    for (i = 0; i < be_count; i++) {
        aif_info = aif_list + i;
        if (aif_info->dir == RX)
            rx_adi->count++;
        else if (aif_info->dir == TX)
            tx_adi->count++;
    }

    rx_adi->names = calloc(rx_adi->count + 1, sizeof(*rx_adi->names));
    rx_adi->idx_arr = calloc(rx_adi->count + 1, sizeof(*rx_adi->idx_arr));
    tx_adi->names = calloc(tx_adi->count + 1, sizeof(*tx_adi->names));
    tx_adi->idx_arr = calloc(tx_adi->count + 1, sizeof(*tx_adi->idx_arr));

    if (!rx_adi->names || !tx_adi->names ||
        !rx_adi->idx_arr || !tx_adi->idx_arr) {
        ret = -ENOMEM;
        goto err_backends_get;
    }

    /* form the rx backends enum array */
    amp_copy_be_names_from_aif_list(aif_list, be_count, rx_adi, RX);
    amp_copy_be_names_from_aif_list(aif_list, be_count, tx_adi, TX);

    amp_priv->aif_list = aif_list;
    return 0;

err_backends_get:
    amp_free_be_dev_info(amp_priv);
    return ret;
}

static int amp_create_pcm_info_from_card(struct amp_dev_info *adi,
            const char *dir, int num_pcms, void **pcm_node_list)
{
    int ret, i, val = 0, idx = 0;

    adi->names[idx] =  "ZERO";
    adi->idx_arr[idx] = 0;
    idx++;

    for (i = 0; i < num_pcms; i++) {
        void *pcm_node = pcm_node_list[i];
        snd_card_def_get_int(pcm_node, dir, &val);
        if (val == 0)
            continue;

        ret = snd_card_def_get_str(pcm_node, "name",
                                   &adi->names[idx]);
        if (ret) {
            printf("%s failed to get name for %s pcm wih idx %d\n",
                   __func__, dir, idx);
            return -EINVAL;
        }
        ret = snd_card_def_get_int(pcm_node, "id",
                                   &adi->idx_arr[idx]);
        if (ret) {
            printf("%s failed to get name for %s pcm with idx %d\n",
                   __func__, dir, idx);
            return -EINVAL;
        }

        idx++;
    }

    adi->count = idx;

    return 0;
}

static int amp_get_pcm_info(struct amp_priv *amp_priv)
{
    struct amp_dev_info *rx_adi = &amp_priv->rx_pcm_devs;
    struct amp_dev_info *tx_adi = &amp_priv->tx_pcm_devs;
    void **pcm_node_list = NULL;
    int num_pcms = 0, num_compr = 0, total_pcms, ret, val = 0, i;

    /* Get both pcm and compressed node count */
    num_pcms = snd_card_def_get_num_node(amp_priv->card_node,
                                         SND_NODE_TYPE_PCM);
    num_compr = snd_card_def_get_num_node(amp_priv->card_node,
                                          SND_NODE_TYPE_COMPR);
    if (num_pcms <= 0 && num_compr <= 0) {
        printf("%s: no pcms(%d)/compr(%d) nodes found for card %u\n",
               __func__, num_pcms, num_compr, amp_priv->card);
        ret = -EINVAL;
        goto done;
    }

    /* It is valid that any card could have just PCMs or just comprs or both */
    if (num_pcms < 0) {
        total_pcms = num_compr;
        num_pcms = 0;
    } else if (num_compr < 0) {
        total_pcms = num_pcms;
        num_compr = 0;
    }
    total_pcms = num_pcms + num_compr;

    pcm_node_list = calloc(total_pcms, sizeof(*pcm_node_list));
    if (!pcm_node_list) {
        printf("%s: alloc for pcm_node_list failed\n", __func__);
        return -ENOMEM;
    }

    if (num_pcms > 0) {
        ret = snd_card_def_get_nodes_for_type(amp_priv->card_node,
                                              SND_NODE_TYPE_PCM,
                                              pcm_node_list, num_pcms);
        if (ret) {
            printf("%s: failed to get pcm node list, err %d\n",
                   __func__, ret);
            goto done;
        }
    }

    if (num_compr > 0) {
        ret = snd_card_def_get_nodes_for_type(amp_priv->card_node,
                                              SND_NODE_TYPE_COMPR,
                                              &pcm_node_list[num_pcms],
                                              num_compr);
        if (ret) {
            printf("%s: failed to get compr node list, err %d\n",
                   __func__, ret);
            goto done;
        }
    }

    /* count TX and RX PCMs + Comprs*/
    for (i = 0; i < total_pcms; i++) {
        void *pcm_node = pcm_node_list[i];
        snd_card_def_get_int(pcm_node, "playback", &val);
        if (val == 1)
            rx_adi->count++;
    }
    val = 0;
    for (i = 0; i < total_pcms; i++) {
        void *pcm_node = pcm_node_list[i];
        snd_card_def_get_int(pcm_node, "capture", &val);
        if (val == 1)
            tx_adi->count++;
    }

    /* Allocate rx and tx structures */
    rx_adi->names = calloc(rx_adi->count + 1, sizeof(*rx_adi->names));
    rx_adi->idx_arr = calloc(rx_adi->count + 1, sizeof(*rx_adi->idx_arr));
    tx_adi->names = calloc(tx_adi->count + 1, sizeof(*tx_adi->names));
    tx_adi->idx_arr = calloc(tx_adi->count + 1, sizeof(*tx_adi->idx_arr));

    if (!rx_adi->names || !tx_adi->names ||
        !rx_adi->idx_arr || !tx_adi->idx_arr) {
        ret = -ENOMEM;
        goto err_alloc_rx_tx;
    }
    
    
    /* Fill in RX properties */
    ret = amp_create_pcm_info_from_card(rx_adi, "playback",
                                        total_pcms, pcm_node_list);
    if (ret)
        goto err_alloc_rx_tx;

    ret = amp_create_pcm_info_from_card(tx_adi, "capture",
                                        total_pcms, pcm_node_list);
    if (ret)
        goto err_alloc_rx_tx;

    rx_adi->dev_enum.items = rx_adi->count;
    rx_adi->dev_enum.texts = rx_adi->names;
    tx_adi->dev_enum.items = tx_adi->count;
    tx_adi->dev_enum.texts = tx_adi->names;

    goto done;

err_alloc_rx_tx:
    amp_free_pcm_dev_info(amp_priv);
    
done:
    if (pcm_node_list)
        free(pcm_node_list);

    return ret;
}

static void amp_register_event_callback(struct mixer_plugin *plugin, int enable)
{
    struct amp_priv *amp_priv = plugin->priv;
    struct amp_dev_info *rx_adi = &amp_priv->rx_pcm_devs;
    struct amp_dev_info *tx_adi = &amp_priv->tx_pcm_devs;
    agm_event_cb cb;
    int idx, session_id;

    if (enable)
        cb = &amp_event_cb;
    else
        cb = NULL;

    for (idx = 1; idx < rx_adi->count; idx++) {
        session_id = rx_adi->idx_arr[idx];
        agm_session_register_cb(session_id, cb, AGM_EVENT_MODULE, plugin);
    }

    for (idx = 1; idx < tx_adi->count; idx++) {
        session_id = tx_adi->idx_arr[idx];
        agm_session_register_cb(session_id, cb, AGM_EVENT_MODULE, plugin);
    }
}

static int amp_get_be_ctl_count(struct amp_priv *amp_priv)
{
    struct amp_dev_info *rx_adi = &amp_priv->rx_be_devs;
    struct amp_dev_info *tx_adi = &amp_priv->tx_be_devs;
    int count, ctl_per_be;

    ctl_per_be = (int)ARRAY_SIZE(amp_be_ctl_name_extn);

    count = 0;

    /* minus 1 is needed to ignore the ZERO string (name) */
    count += (rx_adi->count - 1) * ctl_per_be;
    count += (tx_adi->count - 1) * ctl_per_be;

    return count;
}

static int amp_get_pcm_ctl_count(struct amp_priv *amp_priv)
{
    struct amp_dev_info *rx_adi = &amp_priv->rx_pcm_devs;
    struct amp_dev_info *tx_adi = &amp_priv->tx_pcm_devs;
    int count, ctl_per_pcm;

    count = 0;

    /* Count common ctls applicable for both RX and TX pcms */
    ctl_per_pcm = (int)ARRAY_SIZE(amp_pcm_ctl_name_extn);
    count += (rx_adi->count - 1) * ctl_per_pcm;
    count += (tx_adi->count - 1) * ctl_per_pcm;

    /* Count only TX pcm specific controls */
    ctl_per_pcm = (int)ARRAY_SIZE(amp_pcm_tx_ctl_names);
    count += (tx_adi->count -1) * ctl_per_pcm;

    /* Count only RX pcm specific controls */
    ctl_per_pcm = (int)ARRAY_SIZE(amp_pcm_rx_ctl_names);
    count += (rx_adi->count - 1) * ctl_per_pcm;

    return count;
}

static int amp_pcm_get_control_value(struct amp_priv *amp_priv,
                int pcm_idx, struct amp_dev_info *pcm_adi)
{
    int mtd_idx;

    /* Find the index for metadata_ctl for this pcm */
    for (mtd_idx = 1; mtd_idx < pcm_adi->count; mtd_idx++) {
        if (pcm_idx == pcm_adi->idx_arr[mtd_idx])
            break;
    }

    if (mtd_idx >= pcm_adi->count) {
        printf("%s: metadata index not found for pcm_idx %d",
               __func__, pcm_idx);
        return -EINVAL;
    }

    return pcm_adi->pcm_mtd_ctl[mtd_idx];
}

static int amp_be_media_fmt_get(struct mixer_plugin *plugin,
                struct snd_control *ctl, struct snd_ctl_elem_value *ev)
{
    //TODO: AGM should support get function.
    printf ("%s: enter\n", __func__);
    return 0;
}

static int amp_be_media_fmt_put(struct mixer_plugin *plugin,
                struct snd_control *ctl, struct snd_ctl_elem_value *ev)
{
    struct amp_priv *amp_priv = plugin->priv;
    uint32_t audio_intf_id = ctl->private_value;
    int ret = 0;

    printf ("%s: enter\n", __func__);
    amp_priv->media_fmt.rate = (uint32_t)ev->value.integer.value[0];
    amp_priv->media_fmt.channels = (uint32_t)ev->value.integer.value[1];
    amp_priv->media_fmt.format = alsa_to_agm_fmt(ev->value.integer.value[2]);

    ret = agm_aif_set_media_config(audio_intf_id,
                                   &amp_priv->media_fmt);
    if (ret == -EALREADY)
        ret = 0;

    if (ret)
        printf("%s: set_media_config failed, err %d, aif_id %u rate %u channels %u fmt %u\n",
               __func__, ret, audio_intf_id, amp_priv->media_fmt.rate,
               amp_priv->media_fmt.channels, amp_priv->media_fmt.format);
    return ret;
}

static int amp_be_metadata_get(struct mixer_plugin *plugin,
                struct snd_control *ctl, struct snd_ctl_tlv *tlv)
{
    /* AGM should provide a get */
    printf ("%s: enter\n", __func__);
    return 0;
}

static int amp_be_metadata_put(struct mixer_plugin *plugin,
                struct snd_control *ctl, struct snd_ctl_tlv *tlv)
{
    uint32_t audio_intf_id = ctl->private_value;
    void *payload;
    uint32_t tlv_size;
    int ret;

    payload = &tlv->tlv[0];
    tlv_size = tlv->length;
    printf ("%s: enter\n", __func__);
    ret = agm_aif_set_metadata(audio_intf_id, tlv_size, payload);

    if (ret == -EALREADY)
        ret = 0;

    if (ret)
        printf("%s: set_metadata failed, err %d, aid_id %u\n",
               __func__, ret, audio_intf_id);
    return ret;
}

static int amp_pcm_aif_connect_get(struct mixer_plugin *plugin,
                struct snd_control *ctl, struct snd_ctl_elem_value *ev)
{
    /* TODO: Need AGM support to perform get */
    printf("%s: enter\n", __func__);
    return 0;
}

static int amp_pcm_aif_connect_put(struct mixer_plugin *plugin,
                struct snd_control *ctl, struct snd_ctl_elem_value *ev)
{
    struct amp_priv *amp_priv = plugin->priv;
    struct amp_dev_info *pcm_adi = ctl->private_data;
    struct amp_dev_info *be_adi;
    int be_idx, pcm_idx = ctl->private_value;
    unsigned int val;
    int ret;
    bool state;

    printf ("%s: enter\n", __func__);
    be_adi = amp_get_be_adi(amp_priv, pcm_adi->dir);
    if (!be_adi)
        return -EINVAL;

    val = ev->value.enumerated.item[0];

    /* setting to ZERO is a no-op */
    if (val == 0)
        return 0;

    /*
     * same function caters to connect and disconnect mixer ctl.
     * try to find "disconnect" in the ctl name to differentiate
     * between connect and disconnect mixer ctl.
     */
    if (strstr(ctl->name, "disconnect"))
        state = false;
    else
        state = true;

    be_idx = be_adi->idx_arr[val];
    ret = agm_session_aif_connect(pcm_idx, be_idx, state);
    if (ret == -EALREADY)
        ret = 0;

    if (ret)
        printf("%s: connect failed err %d, pcm_idx %d be_idx %d\n",
               __func__, ret, pcm_idx, be_idx);

    return 0;
}

static int amp_pcm_mtd_control_get(struct mixer_plugin *plugin,
                struct snd_control *ctl, struct snd_ctl_elem_value *ev)
{
    struct amp_dev_info *pcm_adi = ctl->private_data;
    int idx    = ctl->private_value;

    ev->value.enumerated.item[0] = pcm_adi->pcm_mtd_ctl[idx];

    printf ("%s: enter, val = %u\n", __func__,
            ev->value.enumerated.item[0]);
    return 0;
}

static int amp_pcm_mtd_control_put(struct mixer_plugin *plugin,
               struct snd_control *ctl, struct snd_ctl_elem_value *ev)
{
    struct amp_dev_info *pcm_adi = ctl->private_data;
    int idx = ctl->private_value;
    unsigned int val;

    val = ev->value.enumerated.item[0];
    pcm_adi->pcm_mtd_ctl[idx] = val;

    printf("%s: value = %u\n", __func__, val);
    return 0;
}

static int amp_pcm_event_get(struct mixer_plugin *plugin,
                struct snd_control *ctl, struct snd_ctl_elem_value *ev)
{
    struct amp_priv *amp_priv = plugin->priv;
    struct listnode *eparams_node, *temp;
    struct event_params_node *event_node;
    struct agm_event_cb_params *eparams;
    int session_id = ctl->private_value;

    printf ("%s: enter\n", __func__);
    list_for_each_safe(eparams_node, temp, &amp_priv->events_paramlist) {
        event_node = node_to_item(eparams_node, struct event_params_node, node);
	if (event_node->session_id == session_id) {
            eparams = &event_node->event_params;
            memcpy(&ev->value.bytes.data[0], eparams,
                   sizeof(struct agm_event_cb_params) + eparams->event_payload_size);
            list_remove(&event_node->node);
            free(event_node);
            return 0;
        }
    }

    return 0;
}

static int amp_pcm_event_put(struct mixer_plugin *plugin,
                struct snd_control *ctl, struct snd_ctl_elem_value *ev)
{
    struct agm_event_reg_cfg *evt_reg_cfg;
    int session_id = ctl->private_value;
    int ret;

    evt_reg_cfg = (struct agm_event_reg_cfg *) (struct agm_meta_data *) &ev->value.bytes.data[0];
    ret = agm_session_register_for_events(session_id, evt_reg_cfg);
    if (ret == -EALREADY)
        ret = 0;

    if (ret)
        printf("%s: set_event failed, err %d, session_id %u\n",
               __func__, ret, session_id);

    return ret;
}

static int amp_pcm_metadata_get(struct mixer_plugin *plugin,
                struct snd_control *Ctl, struct snd_ctl_tlv *tlv)
{
    /* TODO: AGM needs to provide this in a API */
    printf ("%s: enter\n", __func__);
    return 0;
}

static int amp_pcm_metadata_put(struct mixer_plugin *plugin,
                struct snd_control *ctl, struct snd_ctl_tlv *tlv)
{
    struct amp_dev_info *pcm_adi = ctl->private_data;
    struct amp_dev_info *be_adi;
    int pcm_idx = ctl->private_value;
    int pcm_control, be_idx, ret;
    uint32_t tlv_size;
    void *payload;

    printf("%s: enter\n", __func__);

    pcm_control = amp_pcm_get_control_value(plugin->priv, pcm_idx, pcm_adi);
    if (pcm_control < 0)
        return pcm_control;

    payload = &tlv->tlv[0];
    tlv_size = tlv->length;
    if (pcm_control == 0) {
        ret = agm_session_set_metadata(pcm_idx, tlv_size, payload);
        if (ret == -EALREADY)
            ret = 0;

        if (ret)
            printf("%s: set_session_metadata failed err %d for %s\n",
                   __func__, ret, ctl->name);
        return ret;
    }

    /* pcm control is not 0, set the (session + be) metadata */
    be_adi = amp_get_be_adi(plugin->priv, pcm_adi->dir);
    be_idx = be_adi->idx_arr[pcm_control];
    ret = agm_session_aif_set_metadata(pcm_idx, be_idx, tlv_size, payload);
    if (ret == -EALREADY)
        ret = 0;

    if (ret)
        printf("%s: set_aif_ses_metadata failed err %d for %s\n",
               __func__, ret, ctl->name);
    return ret;
}

static int amp_pcm_calibration_get(struct mixer_plugin *plugin,
                struct snd_control *Ctl, struct snd_ctl_elem_value *ev)
{
    /* TODO: AGM needs to provide this in a API */
    printf ("%s: enter\n", __func__);
    return 0;
}

static int amp_pcm_calibration_put(struct mixer_plugin *plugin,
                struct snd_control *ctl, struct snd_ctl_elem_value *ev)
{
    struct amp_dev_info *pcm_adi = ctl->private_data;
    struct amp_dev_info *be_adi;
    struct agm_cal_config *cal_config;
    int pcm_idx = ctl->private_value;
    int pcm_control, ret, be_idx;

    cal_config = (struct agm_cal_config *) ev->value.bytes.data;

    pcm_control = amp_pcm_get_control_value(plugin->priv, pcm_idx, pcm_adi);
    if (pcm_control < 0)
        return pcm_control;

    be_adi = amp_get_be_adi(plugin->priv, pcm_adi->dir);
    if (!be_adi)
        return -EINVAL;
    be_idx = be_adi->idx_arr[pcm_control];


    printf ("%s: enter sesid:%d audif:%d \n", __func__, pcm_idx, be_idx);
    ret = agm_session_aif_set_cal(pcm_idx, be_idx, cal_config);
    if (ret)
        printf("%s: set_calbration failed, err %d, aif_id %u\n",
               __func__, ret, be_idx);
    return ret;
}

static int amp_pcm_set_param_get(struct mixer_plugin *plugin,
                struct snd_control *ctl, struct snd_ctl_tlv *ev)
{
    /* get of set_param not implemented */
    return 0;
}

static int amp_pcm_set_param_put(struct mixer_plugin *plugin,
                struct snd_control *ctl, struct snd_ctl_tlv *tlv)
{
    struct amp_dev_info *pcm_adi = ctl->private_data;
    struct amp_dev_info *be_adi;
    void *payload;
    int pcm_idx = ctl->private_value;
    int pcm_control, be_idx, ret = 0;
    size_t tlv_size;
    bool is_param_tag = false;

    printf("%s: enter\n", __func__);

    if (strstr(ctl->name, "setParamTag"))
        is_param_tag = true;

    payload = &tlv->tlv[0];
    tlv_size = tlv->length;
    pcm_control = amp_pcm_get_control_value(plugin->priv, pcm_idx, pcm_adi);
    if (pcm_control < 0)
        return pcm_control;

    if (pcm_control == 0) {
        if (is_param_tag) {
            printf("%s: aif not provided for setParamTag\n",
                    __func__);
            return -EINVAL;
        }

        ret = agm_session_set_params(pcm_idx, payload, tlv_size);
        if (ret)
            printf("%s: session_set_params failed err %d for %s\n",
                   __func__, ret, ctl->name);
        return ret;
    }

    /* control is not 0, set the (session + be) set_param */
    be_adi = amp_get_be_adi(plugin->priv, pcm_adi->dir);
    be_idx = be_adi->idx_arr[pcm_control];
    if (is_param_tag)
            ret = agm_set_params_with_tag(pcm_idx, be_idx, payload);
    else
        ret = agm_session_aif_set_params(pcm_idx, be_idx,
                            payload, tlv_size);
    if (ret == -EALREADY)
        ret = 0;

    if (ret)
        printf("%s: set_params failed err %d for %s is_param_tag %s\n",
               __func__, ret, ctl->name, is_param_tag ? "true" : "false");
    return ret;
}

static int amp_pcm_get_param_get(struct mixer_plugin *plugin,
                struct snd_control *ctl, struct snd_ctl_tlv *tlv)
{
    struct amp_dev_info *pcm_adi = ctl->private_data;
    struct amp_dev_info *be_adi;
    void *payload;
    int pcm_idx = ctl->private_value;
    int pcm_control, be_idx, ret = 0;
    size_t tlv_size;

    printf("%s: enter\n", __func__);

    payload = &tlv->tlv[0];
    tlv_size = tlv->length;

    if (!pcm_adi->get_param_payload) {
        printf("%s: put() for getParam not called\n", __func__);
        return -EINVAL;
    }

    if (tlv_size < pcm_adi->get_param_payload_size) {
        printf("%s: Buffer size less than expected\n", __func__);
        return -EINVAL;
    }

    memcpy(payload, pcm_adi->get_param_payload, pcm_adi->get_param_payload_size);
    ret = agm_session_get_params(pcm_idx, payload, tlv_size);

    if (ret == -EALREADY)
        ret = 0;

    if (ret)
        printf("%s: failed err %d for %s\n", __func__, ret, ctl->name);

    free(pcm_adi->get_param_payload);
    pcm_adi->get_param_payload = NULL;
    pcm_adi->get_param_payload_size = 0;
    return ret;
}

static int amp_pcm_get_param_put(struct mixer_plugin *plugin,
                struct snd_control *ctl, struct snd_ctl_tlv *tlv)
{
    struct amp_dev_info *pcm_adi = ctl->private_data;
    void *payload;
    int pcm_idx = ctl->private_value;
    int pcm_control, ret = 0;
    size_t tlv_size;

    printf("%s: enter\n", __func__);

    if (pcm_adi->get_param_payload) {
        free(pcm_adi->get_param_payload);
        pcm_adi->get_param_payload = NULL;
    }
    payload = &tlv->tlv[0];
    pcm_adi->get_param_payload_size = tlv->length;

    pcm_adi->get_param_payload = calloc(1, pcm_adi->get_param_payload_size);
    if (!pcm_adi->get_param_payload)
        return -ENOMEM;

    memcpy(pcm_adi->get_param_payload, payload, pcm_adi->get_param_payload_size);

    return 0;
}

static int amp_pcm_tag_info_get(struct mixer_plugin *plugin,
                struct snd_control *ctl, struct snd_ctl_tlv *tlv)
{
    struct amp_dev_info *pcm_adi = ctl->private_data;
    struct amp_dev_info *be_adi;
    void *payload;
    int pcm_idx = ctl->private_value;
    int pcm_control, be_idx, ret = 0;
    size_t tlv_size, get_size;

    printf("%s: enter\n", __func__);

    pcm_control = amp_pcm_get_control_value(plugin->priv, pcm_idx, pcm_adi);
    if(pcm_control < 0)
        return pcm_control;

    if (pcm_control == 0) {
        printf("%s: cannot get tag info for session only\n",
                __func__);
        return -EINVAL;
    }

    /* control is not 0, get the (session + be) get_tag_info */
    payload = &tlv->tlv[0];
    tlv_size = tlv->length;
    be_adi = amp_get_be_adi(plugin->priv, pcm_adi->dir);
    be_idx = be_adi->idx_arr[pcm_control];

    ret = agm_session_aif_get_tag_module_info(pcm_idx, be_idx,
                    NULL, &get_size);
    if (ret || get_size == 0 || tlv_size < get_size) {
        printf("%s: invalid size, ret %d, tlv_size %ld, get_size %ld\n",
                __func__, ret, tlv_size, get_size);
        return -EINVAL;
    }

    ret = agm_session_aif_get_tag_module_info(pcm_idx, be_idx,
                            payload, &get_size);
    if (ret)
        printf("%s: session_aif_get_tag_module_info failed err %d for %s\n",
               __func__, ret, ctl->name);
    return ret;
}

static int amp_pcm_tag_info_put(struct mixer_plugin *plugin,
                struct snd_control *ctl, struct snd_ctl_tlv *tlv)
{
    /* Set for getTaggedInfo mixer control is not supported */
    return 0;
}

static int amp_pcm_loopback_get(struct mixer_plugin *plugin,
                struct snd_control *Ctl, struct snd_ctl_elem_value *ev)
{
    /* TODO: AGM API not available */
    printf ("%s: enter\n", __func__);
    return 0;
}

static int amp_pcm_loopback_put(struct mixer_plugin *plugin,
                struct snd_control *ctl, struct snd_ctl_elem_value *ev)
{
    struct amp_priv *amp_priv = plugin->priv;
    struct amp_dev_info *pcm_rx_adi;
    int rx_pcm_idx, tx_pcm_idx = ctl->private_value;
    unsigned int val;
    bool state = true;
    int ret;

    printf ("%s: enter\n", __func__);
    pcm_rx_adi = &amp_priv->rx_pcm_devs;
    if (!pcm_rx_adi)
        return -EINVAL;

    val = ev->value.enumerated.item[0];

    /* setting to ZERO is a no-op */
    if (val == 0) {
        rx_pcm_idx = 0;
        state = false;
    } else {
        rx_pcm_idx = pcm_rx_adi->idx_arr[val];
    }

    ret = agm_session_set_loopback(tx_pcm_idx, rx_pcm_idx, state);
    if (ret == -EALREADY)
        ret = 0;

    if (ret)
        printf("%s: loopback failed err %d, tx_pcm_idx %d rx_pcm_idx %d\n",
               __func__, ret, tx_pcm_idx, rx_pcm_idx);

    return 0;
}

static int amp_pcm_echoref_get(struct mixer_plugin *plugin,
                struct snd_control *Ctl, struct snd_ctl_elem_value *ev)
{
    /* TODO: AGM API not available */
    printf ("%s: enter\n", __func__);
    return 0;
}

static int amp_pcm_echoref_put(struct mixer_plugin *plugin,
                struct snd_control *ctl, struct snd_ctl_elem_value *ev)
{
    struct amp_priv *amp_priv = plugin->priv;
    struct amp_dev_info *be_adi;
    int be_idx, pcm_idx = ctl->private_value;
    unsigned int val;
    int ret;
    bool state = true;

    printf ("%s: enter\n", __func__);
    be_adi = amp_get_be_adi(amp_priv, RX);
    if (!be_adi)
        return -EINVAL;

    val = ev->value.enumerated.item[0];

    /* setting to ZERO is reset echoref */
    if (val == 0)
        state = false;

    be_idx = be_adi->idx_arr[val];
    ret = agm_session_set_ec_ref(pcm_idx, be_idx, state);
    if (ret == -EALREADY)
        ret = 0;

    if (ret)
        printf("%s: set ecref failed err %d, pcm_idx %d be_idx %d\n",
               __func__, ret, pcm_idx, be_idx);

    return 0;
}

static int amp_pcm_sidetone_get(struct mixer_plugin *plugin,
                struct snd_control *Ctl, struct snd_ctl_elem_value *ev)
{
    /* TODO: AGM API not available */
    printf ("%s: enter\n", __func__);
    return 0;
}

static int amp_pcm_sidetone_put(struct mixer_plugin *plugin,
                struct snd_control *ctl, struct snd_ctl_elem_value *ev)
{
    //TODO
    return 0;
}

/* 512 max bytes for non-tlv controls, reserving 16 for future use */
static struct snd_value_bytes pcm_event_bytes =
    SND_VALUE_BYTES(512 - 16);
static struct snd_value_bytes pcm_calibration_bytes =
    SND_VALUE_BYTES(512 - 16);
static struct snd_value_tlv_bytes be_metadata_bytes =
    SND_VALUE_TLV_BYTES(1024, amp_be_metadata_get, amp_be_metadata_put);
static struct snd_value_tlv_bytes pcm_metadata_bytes =
    SND_VALUE_TLV_BYTES(1024, amp_pcm_metadata_get, amp_pcm_metadata_put);
static struct snd_value_tlv_bytes pcm_taginfo_bytes =
    SND_VALUE_TLV_BYTES(1024, amp_pcm_tag_info_get, amp_pcm_tag_info_put);
static struct snd_value_tlv_bytes pcm_setparamtag_bytes =
    SND_VALUE_TLV_BYTES(1024, amp_pcm_set_param_get, amp_pcm_set_param_put);
static struct snd_value_tlv_bytes pcm_setparam_bytes =
    SND_VALUE_TLV_BYTES(64 * 1024, amp_pcm_set_param_get, amp_pcm_set_param_put);
static struct snd_value_tlv_bytes pcm_getparam_bytes =
    SND_VALUE_TLV_BYTES(64 * 1024, amp_pcm_get_param_get, amp_pcm_get_param_put);

static struct snd_value_int media_fmt_int =
    SND_VALUE_INTEGER(3, 0, 384000, 1);

/* PCM related mixer controls here */
static void amp_create_connect_ctl(struct amp_priv *amp_priv,
            char *pname, int ctl_idx, struct snd_value_enum *e,
            int pval, void *pdata)
{
    struct snd_control *ctl = AMP_PRIV_GET_CTL_PTR(amp_priv, ctl_idx);
    char *ctl_name = AMP_PRIV_GET_CTL_NAME_PTR(amp_priv, ctl_idx);

    snprintf(ctl_name, AIF_NAME_MAX_LEN + 16, "%s %s",
             pname, amp_pcm_ctl_name_extn[PCM_CTL_NAME_CONNECT]);
    INIT_SND_CONTROL_ENUM(ctl, ctl_name, amp_pcm_aif_connect_get,
                    amp_pcm_aif_connect_put, e, pval, pdata);
}

static void amp_create_disconnect_ctl(struct amp_priv *amp_priv,
            char *pname, int ctl_idx, struct snd_value_enum *e,
            int pval, void *pdata)
{
    struct snd_control *ctl = AMP_PRIV_GET_CTL_PTR(amp_priv, ctl_idx);
    char *ctl_name = AMP_PRIV_GET_CTL_NAME_PTR(amp_priv, ctl_idx);

    snprintf(ctl_name, AIF_NAME_MAX_LEN + 16, "%s %s",
             pname, amp_pcm_ctl_name_extn[PCM_CTL_NAME_DISCONNECT]);
    INIT_SND_CONTROL_ENUM(ctl, ctl_name, amp_pcm_aif_connect_get,
                    amp_pcm_aif_connect_put, e, pval, pdata);
}

static void amp_create_mtd_control_ctl(struct amp_priv *amp_priv,
                char *pname, int ctl_idx, struct snd_value_enum *e,
                int pval, void *pdata)
{
    struct snd_control *ctl = AMP_PRIV_GET_CTL_PTR(amp_priv, ctl_idx);
    char *ctl_name = AMP_PRIV_GET_CTL_NAME_PTR(amp_priv, ctl_idx);

    snprintf(ctl_name, AIF_NAME_MAX_LEN + 16, "%s %s",
             pname, amp_pcm_ctl_name_extn[PCM_CTL_NAME_MTD_CONTROL]);
    INIT_SND_CONTROL_ENUM(ctl, ctl_name, amp_pcm_mtd_control_get,
                    amp_pcm_mtd_control_put, e, pval, pdata);

}

static void amp_create_pcm_event_ctl(struct amp_priv *amp_priv,
                char *name, int ctl_idx, int pval, void *pdata)
{
    struct snd_control *ctl = AMP_PRIV_GET_CTL_PTR(amp_priv, ctl_idx);
    char *ctl_name = AMP_PRIV_GET_CTL_NAME_PTR(amp_priv, ctl_idx);

    snprintf(ctl_name, AIF_NAME_MAX_LEN + 16, "%s %s",
             name, amp_pcm_ctl_name_extn[PCM_CTL_NAME_EVENT]);

    INIT_SND_CONTROL_BYTES(ctl, ctl_name, amp_pcm_event_get,
                    amp_pcm_event_put, pcm_event_bytes,
                    pval, pdata);
}

static void amp_create_pcm_metadata_ctl(struct amp_priv *amp_priv,
                char *name, int ctl_idx, int pval, void *pdata)
{
    struct snd_control *ctl = AMP_PRIV_GET_CTL_PTR(amp_priv, ctl_idx);
    char *ctl_name = AMP_PRIV_GET_CTL_NAME_PTR(amp_priv, ctl_idx);

    snprintf(ctl_name, AIF_NAME_MAX_LEN + 16, "%s %s",
             name, amp_pcm_ctl_name_extn[PCM_CTL_NAME_METADATA]);

    INIT_SND_CONTROL_TLV_BYTES(ctl, ctl_name, pcm_metadata_bytes,
                    pval, pdata);
}

static void amp_create_pcm_set_param_ctl(struct amp_priv *amp_priv,
                char *name, int ctl_idx, int pval, void *pdata,
                bool istagged_setparam)
{
    struct snd_control *ctl = AMP_PRIV_GET_CTL_PTR(amp_priv, ctl_idx);
    char *ctl_name = AMP_PRIV_GET_CTL_NAME_PTR(amp_priv, ctl_idx);

    if (!istagged_setparam) {
        snprintf(ctl_name, AIF_NAME_MAX_LEN + 16, "%s %s",
             name, amp_pcm_ctl_name_extn[PCM_CTL_NAME_SET_PARAM]);
        INIT_SND_CONTROL_TLV_BYTES(ctl, ctl_name, pcm_setparam_bytes,
                    pval, pdata);
    } else {
        snprintf(ctl_name, AIF_NAME_MAX_LEN + 16, "%s %s",
             name, amp_pcm_ctl_name_extn[PCM_CTL_NAME_SET_PARAM_TAG]);
        INIT_SND_CONTROL_TLV_BYTES(ctl, ctl_name, pcm_setparamtag_bytes,
                    pval, pdata);
    }

}

static void amp_create_pcm_get_param_ctl(struct amp_priv *amp_priv,
                char *name, int ctl_idx, int pval, void *pdata)
{
    struct snd_control *ctl = AMP_PRIV_GET_CTL_PTR(amp_priv, ctl_idx);
    char *ctl_name = AMP_PRIV_GET_CTL_NAME_PTR(amp_priv, ctl_idx);

    snprintf(ctl_name, AIF_NAME_MAX_LEN + 16, "%s %s",
         name, amp_pcm_ctl_name_extn[PCM_CTL_NAME_GET_PARAM]);
    INIT_SND_CONTROL_TLV_BYTES(ctl, ctl_name, pcm_getparam_bytes,
                pval, pdata);

}

static void amp_create_pcm_get_tag_info_ctl(struct amp_priv *amp_priv,
                char *name, int ctl_idx, int pval, void *pdata)
{
    struct snd_control *ctl = AMP_PRIV_GET_CTL_PTR(amp_priv, ctl_idx);
    char *ctl_name = AMP_PRIV_GET_CTL_NAME_PTR(amp_priv, ctl_idx);

    snprintf(ctl_name, AIF_NAME_MAX_LEN + 16, "%s %s",
             name, amp_pcm_ctl_name_extn[PCM_CTL_NAME_GET_TAG_INFO]);

    INIT_SND_CONTROL_TLV_BYTES(ctl, ctl_name, pcm_taginfo_bytes,
                    pval, pdata);
}

/* TX only mixer control creations here */
static void amp_create_pcm_loopback_ctl(struct amp_priv *amp_priv,
            char *pname, int ctl_idx, struct snd_value_enum *e,
            int pval, void *pdata)
{
    struct snd_control *ctl = AMP_PRIV_GET_CTL_PTR(amp_priv, ctl_idx);
    char *ctl_name = AMP_PRIV_GET_CTL_NAME_PTR(amp_priv, ctl_idx);

    snprintf(ctl_name, AIF_NAME_MAX_LEN + 16, "%s %s",
             pname, amp_pcm_tx_ctl_names[PCM_TX_CTL_NAME_LOOPBACK]);
    INIT_SND_CONTROL_ENUM(ctl, ctl_name, amp_pcm_loopback_get,
                    amp_pcm_loopback_put, e, pval, pdata);
}

static void amp_create_pcm_echoref_ctl(struct amp_priv *amp_priv,
            char *pname, int ctl_idx, struct snd_value_enum *e,
            int pval, void *pdata)
{
    struct snd_control *ctl = AMP_PRIV_GET_CTL_PTR(amp_priv, ctl_idx);
    char *ctl_name = AMP_PRIV_GET_CTL_NAME_PTR(amp_priv, ctl_idx);

    snprintf(ctl_name, AIF_NAME_MAX_LEN + 16, "%s %s",
             pname, amp_pcm_tx_ctl_names[PCM_TX_CTL_NAME_ECHOREF]);
    INIT_SND_CONTROL_ENUM(ctl, ctl_name, amp_pcm_echoref_get,
                    amp_pcm_echoref_put, e, pval, pdata);
}

/* RX only mixer control creations here */
static void amp_create_pcm_sidetone_ctl(struct amp_priv *amp_priv,
            char *pname, int ctl_idx, struct snd_value_enum *e,
            int pval, void *pdata)
{
    struct snd_control *ctl = AMP_PRIV_GET_CTL_PTR(amp_priv, ctl_idx);
    char *ctl_name = AMP_PRIV_GET_CTL_NAME_PTR(amp_priv, ctl_idx);

    snprintf(ctl_name, AIF_NAME_MAX_LEN + 16, "%s %s",
             pname, amp_pcm_rx_ctl_names[PCM_RX_CTL_NAME_SIDETONE]);
    INIT_SND_CONTROL_ENUM(ctl, ctl_name, amp_pcm_sidetone_get,
                    amp_pcm_sidetone_put, e, pval, pdata);
}


static void amp_create_pcm_calibration_ctl(struct amp_priv *amp_priv,
                char *name, int ctl_idx, int pval, void *pdata)
{
    struct snd_control *ctl = AMP_PRIV_GET_CTL_PTR(amp_priv, ctl_idx);
    char *ctl_name = AMP_PRIV_GET_CTL_NAME_PTR(amp_priv, ctl_idx);

    snprintf(ctl_name, AIF_NAME_MAX_LEN + 16, "%s %s",
             name, amp_pcm_ctl_name_extn[PCM_CTL_NAME_SET_CALIBRATION]);

    INIT_SND_CONTROL_BYTES(ctl, ctl_name, amp_pcm_calibration_get,
                    amp_pcm_calibration_put, pcm_calibration_bytes,
                    pval, pdata);
}

/* BE related mixer control creations here */
static void amp_create_metadata_ctl(struct amp_priv *amp_priv,
                char *be_name, int ctl_idx, int pval, void *pdata)
{
    struct snd_control *ctl = AMP_PRIV_GET_CTL_PTR(amp_priv, ctl_idx);
    char *ctl_name = AMP_PRIV_GET_CTL_NAME_PTR(amp_priv, ctl_idx);

    snprintf(ctl_name, AIF_NAME_MAX_LEN + 16, "%s %s",
             be_name, amp_be_ctl_name_extn[BE_CTL_NAME_METADATA]);

    INIT_SND_CONTROL_TLV_BYTES(ctl, ctl_name, be_metadata_bytes,
                    pval, pdata);
}

static void amp_create_media_fmt_ctl(struct amp_priv *amp_priv,
                char *be_name, int ctl_idx, int pval, void *pdata)
{
    struct snd_control *ctl = AMP_PRIV_GET_CTL_PTR(amp_priv, ctl_idx);
    char *ctl_name = AMP_PRIV_GET_CTL_NAME_PTR(amp_priv, ctl_idx);

    snprintf(ctl_name, AIF_NAME_MAX_LEN + 16, "%s %s",
             be_name, amp_be_ctl_name_extn[BE_CTL_NAME_MEDIA_CONFIG]);
    INIT_SND_CONTROL_INTEGER(ctl, ctl_name, amp_be_media_fmt_get,
                    amp_be_media_fmt_put, media_fmt_int, pval, pdata);
}

static int amp_form_be_ctls(struct amp_priv *amp_priv, int ctl_idx, int ctl_cnt)
{
    struct amp_dev_info *rx_adi = &amp_priv->rx_be_devs;
    struct amp_dev_info *tx_adi = &amp_priv->tx_be_devs;
    int i;

    for (i = 1; i < rx_adi->count; i++) {
        amp_create_media_fmt_ctl(amp_priv, rx_adi->names[i], ctl_idx,
                                 rx_adi->idx_arr[i], rx_adi);
        ctl_idx++;
        amp_create_metadata_ctl(amp_priv, rx_adi->names[i], ctl_idx,
                                rx_adi->idx_arr[i], rx_adi);
        ctl_idx++;
    }

    for (i = 1; i < tx_adi->count; i++) {
        amp_create_media_fmt_ctl(amp_priv, tx_adi->names[i], ctl_idx,
                                 tx_adi->idx_arr[i], tx_adi);
        ctl_idx++;
        amp_create_metadata_ctl(amp_priv, tx_adi->names[i], ctl_idx,
                                tx_adi->idx_arr[i], tx_adi);
        ctl_idx++;
    }

    return 0;
}

static int amp_form_common_pcm_ctls(struct amp_priv *amp_priv, int *ctl_idx,
                struct amp_dev_info *pcm_adi, struct amp_dev_info *be_adi)
{
    int i;

    for (i = 1; i < pcm_adi->count; i++) {
        char *name = pcm_adi->names[i];
        int idx = pcm_adi->idx_arr[i];
        amp_create_connect_ctl(amp_priv, name, (*ctl_idx)++,
                        &be_adi->dev_enum, idx, pcm_adi);
        amp_create_disconnect_ctl(amp_priv, name, (*ctl_idx)++,
                        &be_adi->dev_enum, idx, pcm_adi);
        pcm_adi->pcm_mtd_ctl = calloc(pcm_adi->count, sizeof(int));
        if (!pcm_adi->pcm_mtd_ctl)
            return -ENOMEM;
        amp_create_mtd_control_ctl(amp_priv, name, (*ctl_idx)++,
                        &be_adi->dev_enum, i, pcm_adi);
        amp_create_pcm_metadata_ctl(amp_priv, name, (*ctl_idx)++,
                        idx, pcm_adi);
        amp_create_pcm_set_param_ctl(amp_priv, name, (*ctl_idx)++,
                        idx, pcm_adi, false);
        amp_create_pcm_set_param_ctl(amp_priv, name, (*ctl_idx)++,
                        idx, pcm_adi, true);
        amp_create_pcm_get_tag_info_ctl(amp_priv, name, (*ctl_idx)++,
                        idx, pcm_adi);
        amp_create_pcm_event_ctl(amp_priv, name, (*ctl_idx)++,
                        idx, pcm_adi);
        amp_create_pcm_calibration_ctl(amp_priv, name, (*ctl_idx)++,
                        idx, pcm_adi);
        amp_create_pcm_get_param_ctl(amp_priv, name, (*ctl_idx)++,
                        idx, pcm_adi);
    }

    return 0;
}

static int amp_form_tx_pcm_ctls(struct amp_priv *amp_priv, int *ctl_idx)
{
    struct amp_dev_info *rx_adi = &amp_priv->rx_pcm_devs;
    struct amp_dev_info *tx_adi = &amp_priv->tx_pcm_devs;
    struct amp_dev_info *be_rx_adi = &amp_priv->rx_be_devs;
    int i;

    for (i = 1; i < tx_adi->count; i++) {
        char *name = tx_adi->names[i];
        int idx = tx_adi->idx_arr[i];

        /* create loopback controls, enum values are RX PCMs*/
        amp_create_pcm_loopback_ctl(amp_priv, name, (*ctl_idx)++,
                        &rx_adi->dev_enum, idx, tx_adi);
        /* Echo Reference has backend RX as enum values */
        amp_create_pcm_echoref_ctl(amp_priv, name, (*ctl_idx)++,
                        &be_rx_adi->dev_enum, idx, tx_adi);
    }

    return 0;
}

static int amp_form_rx_pcm_ctls(struct amp_priv *amp_priv, int *ctl_idx)
{
    struct amp_dev_info *rx_adi = &amp_priv->rx_pcm_devs;
    struct amp_dev_info *be_tx_adi = &amp_priv->tx_be_devs;
    int i;

    for (i = 1; i < rx_adi->count; i++) {
        char *name = rx_adi->names[i];
        int idx = rx_adi->idx_arr[i];

        /* Create sidetone control, enum values are TX backends */
        amp_create_pcm_sidetone_ctl(amp_priv, name, (*ctl_idx)++,
                        &be_tx_adi->dev_enum, idx, rx_adi);
    }

    return 0;
}

static int amp_form_pcm_ctls(struct amp_priv *amp_priv, int ctl_idx, int ctl_cnt)
{
    struct amp_dev_info *rx_adi = &amp_priv->rx_pcm_devs;
    struct amp_dev_info *tx_adi = &amp_priv->tx_pcm_devs;
    struct amp_dev_info *be_rx_adi = &amp_priv->rx_be_devs;
    struct amp_dev_info *be_tx_adi = &amp_priv->tx_be_devs;
    int ret;

    /* Form common controls for RX pcms */
    ret = amp_form_common_pcm_ctls(amp_priv, &ctl_idx, rx_adi, be_rx_adi);
    if (ret)
        return ret;

    /* Form RX PCM specific mixer controls */
    ret = amp_form_rx_pcm_ctls(amp_priv, &ctl_idx);
    if (ret)
        return ret;

    /* Form common controls for TX pcms */
    ret = amp_form_common_pcm_ctls(amp_priv, &ctl_idx, tx_adi, be_tx_adi);
    if (ret)
        return ret;

    /* Form TX PCM specific mixer controls */
    ret = amp_form_tx_pcm_ctls(amp_priv, &ctl_idx);
    if (ret)
        return ret;

    return 0;
}

static ssize_t amp_read_event(struct mixer_plugin *plugin,
                              struct snd_ctl_event *ev, size_t size)
{
    struct amp_priv *amp_priv = plugin->priv;
    struct listnode *ev_node, *temp;
    ssize_t result = 0;

    while (size >= sizeof(struct snd_ctl_event)) {
        struct mixer_plugin_event_data *data;

        if (list_empty(&amp_priv->events_list))
            return result;

        data = node_to_item(amp_priv->events_list.next, struct mixer_plugin_event_data, node);
        memcpy(ev, &data->ev, sizeof(struct snd_ctl_event));

        list_remove(&data->node);
        free(data);
        ev += sizeof(struct snd_ctl_event);
        size -= sizeof(struct snd_ctl_event);
        result += sizeof(struct snd_ctl_event);
    }

    return result;
}

static int amp_subscribe_events(struct mixer_plugin *plugin,
                                  event_callback event_cb)
{
    struct amp_priv *amp_priv = plugin->priv;
    struct listnode *eparams_node, *ev_node, *temp, *temp2;
    struct event_params_node *event_node;
    struct mixer_plugin_event_data *ev_data;

    printf ("%s: enter\n", __func__);

    amp_priv->event_cb = event_cb;

    /* clear all event params on unsubscribe */
    if (event_cb == NULL) {
        list_for_each_safe(eparams_node, temp, &amp_priv->events_paramlist) {
            event_node = node_to_item(eparams_node, struct event_params_node, node);
            list_remove(&event_node->node);
            free(event_node);
        }

        list_for_each_safe(ev_node, temp2, &amp_priv->events_list) {
            ev_data = node_to_item(ev_node, struct mixer_plugin_event_data, node);
            list_remove(&ev_data->node);
            free(ev_data);
        }
    }
    return 0;
}

static void amp_close(struct mixer_plugin **plugin)
{
    struct mixer_plugin *amp = *plugin;
    struct amp_priv *amp_priv = amp->priv;

    amp_register_event_callback(amp, 0);
    amp_subscribe_events(amp, NULL);
    snd_card_def_put_card(amp_priv->card_node);
    amp_free_pcm_dev_info(amp_priv);
    amp_free_be_dev_info(amp_priv);
    amp_free_ctls(amp_priv);
    free(amp_priv);
    free(*plugin);
    plugin = NULL;
}

struct mixer_plugin_ops amp_ops = {
    .close = amp_close,
    .subscribe_events = amp_subscribe_events,
    .read_event = amp_read_event,
};

MIXER_PLUGIN_OPEN_FN(agm_mixer_plugin)
{
    struct mixer_plugin *amp;
    struct amp_priv *amp_priv;
    struct pcm_adi;
    int ret = 0;
    int be_ctl_cnt, pcm_ctl_cnt, total_ctl_cnt = 0;

    printf("%s: enter, card %u\n", __func__, card);

    amp = calloc(1, sizeof(*amp));
    if (!amp) {
        printf("agm mixer plugin alloc failed\n");
        return -ENOMEM;
    }

    amp_priv = calloc(1, sizeof(*amp_priv));
    if (!amp_priv) {
        printf("amp priv data alloc failed\n");
        ret = -ENOMEM;
        goto err_priv_alloc;
    }

    amp_priv->card = card;
    amp_priv->card_node = snd_card_def_get_card(amp_priv->card);
    if (!amp_priv->card_node) {
        printf("%s: card node not found for card %d\n",
               __func__, amp_priv->card);
        ret = -EINVAL;
        goto err_get_card;
    }

    amp_priv->rx_be_devs.dir = RX;
    amp_priv->tx_be_devs.dir = TX;
    amp_priv->rx_pcm_devs.dir = RX;
    amp_priv->tx_pcm_devs.dir = TX;

    ret = amp_get_be_info(amp_priv);
    if (ret)
        goto err_get_be_info;
    ret = amp_get_pcm_info(amp_priv);
    if (ret)
        goto err_get_pcm_info;

    /* Get total count of controls to be registered */
    be_ctl_cnt = amp_get_be_ctl_count(amp_priv);
    total_ctl_cnt += be_ctl_cnt;
    pcm_ctl_cnt = amp_get_pcm_ctl_count(amp_priv);
    total_ctl_cnt += pcm_ctl_cnt;

    /*
     * Create the controls to be registered
     * When changing this code, be careful to make sure to create
     * exactly the same number of controls as of total_ctl_cnt;
     */
    amp_priv->ctls = calloc(total_ctl_cnt, sizeof(*amp_priv->ctls));
    amp_priv->ctl_names = calloc(total_ctl_cnt, sizeof(*amp_priv->ctl_names));
    if (!amp_priv->ctls || !amp_priv->ctl_names)
            goto err_ctls_alloc;

    ret = amp_form_be_ctls(amp_priv, 0, be_ctl_cnt);
    if (ret)
        goto err_ctls_alloc;
    ret = amp_form_pcm_ctls(amp_priv, be_ctl_cnt, pcm_ctl_cnt);
    if (ret)
        goto err_ctls_alloc;

    /* Register the controls */
    if (total_ctl_cnt > 0) {
        amp_priv->ctl_count = total_ctl_cnt;
        amp->controls = amp_priv->ctls;
        amp->num_controls = amp_priv->ctl_count;
    }

    amp->ops = &amp_ops;
    amp->priv = amp_priv;
    *plugin = amp;

    amp_register_event_callback(amp, 1);
    list_init(&amp_priv->events_paramlist);
    list_init(&amp_priv->events_list);
    printf("%s: total_ctl_cnt = %d\n", __func__, total_ctl_cnt);

    return 0;

err_ctls_alloc:
    amp_free_ctls(amp_priv);
    amp_free_pcm_dev_info(amp_priv);

err_get_pcm_info:
    amp_free_be_dev_info(amp_priv);

err_get_be_info:
    snd_card_def_put_card(amp_priv->card_node);

err_get_card:
    free(amp_priv);

err_priv_alloc:
    free(amp);
    return -ENOMEM;
}
