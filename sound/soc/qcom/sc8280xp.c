// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2022, Linaro Limited

#include <dt-bindings/sound/qcom,q6afe.h>
#include <dt-bindings/sound/qcom,q6dsp-lpass-ports.h>
#include <linux/clk.h>
#include <linux/input-event-codes.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/soundwire/sdw.h>
#include <sound/jack.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include "qdsp6/q6afe.h"
#include "qdsp6/q6prm.h"
#include "common.h"
#include "sdw.h"

struct sc8280xp_snd_data {
	bool stream_prepared[AFE_PORT_MAX];
	struct snd_soc_card *card;
	struct snd_soc_jack jack;
	struct snd_soc_jack dp_jack[8];
	bool jack_setup;
	/* TDM slot config per DAI id, read from DT */
	unsigned int tdm_slot_width[AFE_PORT_MAX];
	unsigned int tdm_nslots[AFE_PORT_MAX];
	/* AUD_INTF CCF clocks, one per TDM interface (0=PRI..4=QUIN) */
	struct clk *tdm_clk[5];
};

static int sc8280xp_snd_init(struct snd_soc_pcm_runtime *rtd)
{
	struct sc8280xp_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_soc_card *card = rtd->card;
	struct snd_soc_jack *dp_jack  = NULL;
	int dp_pcm_id = 0;

	switch (cpu_dai->id) {
	case PRIMARY_MI2S_RX...QUATERNARY_MI2S_TX:
	case QUINARY_MI2S_RX...QUINARY_MI2S_TX:
		snd_soc_dai_set_fmt(cpu_dai, SND_SOC_DAIFMT_BP_FP);
		break;
	case PRIMARY_TDM_RX_0 ... QUINARY_TDM_TX_7: {
		unsigned int nslots = 2, slot_width = 32;

		of_property_read_u32(rtd->card->dev->of_node,
				     "qcom,tdm-slots", &nslots);
		of_property_read_u32(rtd->card->dev->of_node,
				     "qcom,tdm-slot-width", &slot_width);

		data->tdm_nslots[cpu_dai->id] = nslots;
		data->tdm_slot_width[cpu_dai->id] = slot_width;

		snd_soc_dai_set_fmt(cpu_dai, SND_SOC_DAIFMT_BP_FP);
		break;
	}
	case WSA_CODEC_DMA_RX_0:
	case WSA_CODEC_DMA_RX_1:
		/*
		 * Set limit of -3 dB on Digital Volume and 0 dB on PA Volume
		 * to reduce the risk of speaker damage until we have active
		 * speaker protection in place.
		 */
		snd_soc_limit_volume(card, "WSA_RX0 Digital Volume", 81);
		snd_soc_limit_volume(card, "WSA_RX1 Digital Volume", 81);
		snd_soc_limit_volume(card, "SpkrLeft PA Volume", 17);
		snd_soc_limit_volume(card, "SpkrRight PA Volume", 17);
		break;
	case DISPLAY_PORT_RX_0:
		/* DISPLAY_PORT dai ids are not contiguous */
		dp_pcm_id = 0;
		dp_jack = &data->dp_jack[dp_pcm_id];
		break;
	case DISPLAY_PORT_RX_1 ... DISPLAY_PORT_RX_7:
		dp_pcm_id = cpu_dai->id - DISPLAY_PORT_RX_1 + 1;
		dp_jack = &data->dp_jack[dp_pcm_id];
		break;
	default:
		break;
	}

	if (dp_jack)
		return qcom_snd_dp_jack_setup(rtd, dp_jack, dp_pcm_id);

	return qcom_snd_wcd_jack_setup(rtd, &data->jack, &data->jack_setup);
}

static int sc8280xp_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
				     struct snd_pcm_hw_params *params)
{
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_CHANNELS);
	struct snd_mask *fmt = hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT);

	rate->min = rate->max = 48000;
	snd_mask_set_format(fmt, SNDRV_PCM_FORMAT_S16_LE);
	channels->min = 2;
	channels->max = 2;
	switch (cpu_dai->id) {
	case VA_CODEC_DMA_TX_0:
	case TX_CODEC_DMA_TX_0:
	case TX_CODEC_DMA_TX_1:
	case TX_CODEC_DMA_TX_2:
	case TX_CODEC_DMA_TX_3:
		channels->min = 1;
		break;
	default:
		break;
	}

	return 0;
}

/*
 * Map TDM DAI id to the corresponding LPASS AUD_INTF IBIT clock ID.
 * Hawi uses AUD_INTF{N}_IBIT clocks (0x500+) instead of legacy PRI_TDM_IBIT.
 * Each TDM interface spans 16 DAI IDs (8 RX + 8 TX).
 */
static int sc8280xp_tdm_get_clk_id(int dai_id)
{
	if (dai_id >= PRIMARY_TDM_RX_0 && dai_id <= PRIMARY_TDM_RX_0 + 15)
		return Q6PRM_LPASS_CLK_ID_AUD_INTF0_IBIT;
	if (dai_id >= SECONDARY_TDM_RX_0 && dai_id <= SECONDARY_TDM_RX_0 + 15)
		return Q6PRM_LPASS_CLK_ID_AUD_INTF1_IBIT;
	if (dai_id >= TERTIARY_TDM_RX_0 && dai_id <= TERTIARY_TDM_RX_0 + 15)
		return Q6PRM_LPASS_CLK_ID_AUD_INTF2_IBIT;
	if (dai_id >= QUATERNARY_TDM_RX_0 && dai_id <= QUATERNARY_TDM_RX_0 + 15)
		return Q6PRM_LPASS_CLK_ID_AUD_INTF3_IBIT;
	if (dai_id >= QUINARY_TDM_RX_0 && dai_id <= QUINARY_TDM_RX_0 + 15)
		return Q6PRM_LPASS_CLK_ID_AUD_INTF4_IBIT;
	return -EINVAL;
}

static int sc8280xp_tdm_snd_prepare(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);
	struct sc8280xp_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	unsigned int rate = substream->runtime->rate;
	unsigned int slot_width, nslots, clk_freq;
	int intf_idx;
	int ret;

	intf_idx = sc8280xp_tdm_get_clk_id(cpu_dai->id);
	if (intf_idx < 0)
		return intf_idx;

	/* Map AUD_INTF index from clk_id offset */
	intf_idx = (intf_idx - Q6PRM_LPASS_CLK_ID_AUD_INTF0_IBIT) / 2;
	if (intf_idx < 0 || intf_idx >= ARRAY_SIZE(data->tdm_clk))
		return -EINVAL;

	slot_width = data->tdm_slot_width[cpu_dai->id] ?
		     data->tdm_slot_width[cpu_dai->id] : 32;
	nslots = data->tdm_nslots[cpu_dai->id] ?
		 data->tdm_nslots[cpu_dai->id] : 8;

	intf_idx = 2;
	/* clk_freq = sample_rate * slot_width * nslots_per_frame */
	clk_freq = rate * slot_width * nslots;

	if (!data->tdm_clk[intf_idx]) {
		dev_dbg(rtd->dev, "no AUD_INTF clock for intf%d, skipping\n",
			intf_idx);
		return 0;
	}

	ret = clk_set_rate(data->tdm_clk[intf_idx], clk_freq);
	if (ret) {
		dev_err(rtd->dev, "Failed to set TDM clock rate %u: %d\n",
			clk_freq, ret);
		return ret;
	}

	ret = clk_prepare_enable(data->tdm_clk[intf_idx]);
	if (ret)
		return ret;

	if (codec_dai && strnstr(codec_dai->name, "wsa885x", strlen(codec_dai->name))) {
		unsigned int tdm_clk = rate * 0x02 * slot_width;

		snd_soc_dai_set_tdm_slot(cpu_dai, 0x0f, 0b11, 0x02, slot_width);
		snd_soc_dai_set_sysclk(codec_dai, 0, tdm_clk, 0);
	}

	return 0;
}

static int sc8280xp_tdm_snd_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct sc8280xp_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	int intf_idx;

	intf_idx = sc8280xp_tdm_get_clk_id(cpu_dai->id);
	if (intf_idx < 0)
		return intf_idx;

	intf_idx = (intf_idx - Q6PRM_LPASS_CLK_ID_AUD_INTF0_IBIT) / 2;
	if (intf_idx < 0 || intf_idx >= ARRAY_SIZE(data->tdm_clk))
		return -EINVAL;

	if (data->tdm_clk[intf_idx])
		clk_disable_unprepare(data->tdm_clk[intf_idx]);

	return 0;
}

static int sc8280xp_snd_prepare(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct sc8280xp_snd_data *data = snd_soc_card_get_drvdata(rtd->card);

	/*
	 * For TDM DAI IDs, enable the AUD_INTF IBIT clock at the computed
	 * bit-clock frequency (sample_rate * slot_width * nslots).
	 * For SoundWire DAI IDs, delegate to the SDW helper which handles
	 * stream prepare/enable. The SDW helper already guards itself with
	 * qcom_snd_is_sdw_dai(), so it is safe to call for any DAI ID.
	 */
	if (cpu_dai->id >= PRIMARY_TDM_RX_0 && cpu_dai->id <= QUINARY_TDM_TX_7)
		return sc8280xp_tdm_snd_prepare(substream);

	return qcom_snd_sdw_prepare(substream, &data->stream_prepared[cpu_dai->id]);
}

static int sc8280xp_snd_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct sc8280xp_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);

	/* TDM: disable the AUD_INTF IBIT clock */
	if (cpu_dai->id >= PRIMARY_TDM_RX_0 && cpu_dai->id <= QUINARY_TDM_TX_7)
		return sc8280xp_tdm_snd_hw_free(substream);

	return qcom_snd_sdw_hw_free(substream, &data->stream_prepared[cpu_dai->id]);
}

static const struct snd_soc_ops sc8280xp_be_ops = {
	.startup = qcom_snd_sdw_startup,
	.shutdown = qcom_snd_sdw_shutdown,
	.hw_free = sc8280xp_snd_hw_free,
	.prepare = sc8280xp_snd_prepare,
};

static void sc8280xp_add_be_ops(struct snd_soc_card *card)
{
	struct snd_soc_dai_link *link;
	int i;

	for_each_card_prelinks(card, i, link) {
		if (link->no_pcm == 1) {
			link->init = sc8280xp_snd_init;
			link->be_hw_params_fixup = sc8280xp_be_hw_params_fixup;
			link->ops = &sc8280xp_be_ops;
		}
	}
}

static int sc8280xp_platform_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card;
	struct sc8280xp_snd_data *data;
	struct device *dev = &pdev->dev;
	int ret;

	card = devm_kzalloc(dev, sizeof(*card), GFP_KERNEL);
	if (!card)
		return -ENOMEM;
	card->owner = THIS_MODULE;
	/* Allocate the private data */
	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	card->dev = dev;
	dev_set_drvdata(dev, card);
	snd_soc_card_set_drvdata(card, data);
	ret = qcom_snd_parse_of(card);
	if (ret)
		return ret;

	card->driver_name = of_device_get_match_data(dev);

	/* Acquire AUD_INTF IBIT clocks for TDM interfaces (optional, Hawi) */
	{
		int i;
		static const char * const clk_names[] = {
			"aud-intf0-ibit", "aud-intf1-ibit", "aud-intf2-ibit",
			"aud-intf3-ibit", "aud-intf4-ibit",
		};

		for (i = 0; i < ARRAY_SIZE(data->tdm_clk); i++) {
			data->tdm_clk[i] = devm_clk_get_optional(dev, clk_names[i]);
			if (IS_ERR(data->tdm_clk[i]))
				return PTR_ERR(data->tdm_clk[i]);
		}
	}

	sc8280xp_add_be_ops(card);
	return devm_snd_soc_register_card(dev, card);
}

static const struct of_device_id snd_sc8280xp_dt_match[] = {
	{.compatible = "qcom,kaanapali-sndcard", "kaanapali"},
	{.compatible = "qcom,qcm6490-idp-sndcard", "qcm6490"},
	{.compatible = "qcom,qcs615-sndcard", "qcs615"},
	{.compatible = "qcom,qcs6490-rb3gen2-sndcard", "qcs6490"},
	{.compatible = "qcom,qcs8275-sndcard", "qcs8300"},
	{.compatible = "qcom,qcs9075-sndcard", "sa8775p"},
	{.compatible = "qcom,qcs9100-sndcard", "sa8775p"},
	{.compatible = "qcom,sc8280xp-sndcard", "sc8280xp"},
	{.compatible = "qcom,sm8450-sndcard", "sm8450"},
	{.compatible = "qcom,sm8550-sndcard", "sm8550"},
	{.compatible = "qcom,sm8650-sndcard", "sm8650"},
	{.compatible = "qcom,sm8750-sndcard", "sm8750"},
	{.compatible = "qcom,shikra-sndcard", "shikra"},
	{}
};

MODULE_DEVICE_TABLE(of, snd_sc8280xp_dt_match);

static struct platform_driver snd_sc8280xp_driver = {
	.probe  = sc8280xp_platform_probe,
	.driver = {
		.name = "snd-sc8280xp",
		.of_match_table = snd_sc8280xp_dt_match,
	},
};
module_platform_driver(snd_sc8280xp_driver);
MODULE_AUTHOR("Srinivas Kandagatla <srinivas.kandagatla@linaro.org");
MODULE_DESCRIPTION("SC8280XP ASoC Machine Driver");
MODULE_LICENSE("GPL");
