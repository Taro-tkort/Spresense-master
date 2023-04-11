//some standars includes
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <asmp/mpshm.h>
#include <arch/chip/pm.h>
#include <arch/board/board.h>
#include <sys/stat.h>

//audio
#include <audio/audio_high_level_api.h>
#include <audio/audio_player_api.h>
#include <memutils/simple_fifo/CMN_SimpleFifo.h>
#include <memutils/memory_manager/MemHandle.h>
#include <memutils/message/Message.h>
#include "include/msgq_id.h"
#include "include/mem_layout.h"
#include "include/memory_layout.h"
#include "include/msgq_pool.h"
#include "include/pool_layout.h"
#include "include/fixed_fence.h"
#include <audio/audio_message_types.h>

//playlist
#ifdef CONFIG_AUDIOUTILS_PLAYLIST
#include <audio/utilities/playlist.h>
#endif
#ifdef CONFIG_EXAMPLES_AUDIO_PLAYER_USEPOSTPROC
#include "userproc_command.h"
#endif // CONFIG_EXAMPLES_AUDIO_PLAYER_USEPOSTPROC

//section the memory
#define AUDIO_SECTION   SECTION_NO0
using namespace MemMgrLite;



//Things needed for the audio playback
#define PLAYBACK_FILE_PATH "/mnt/sd0/AUDIO"
#define DSPBIN_FILE_PATH   "/mnt/sd0/BIN"
#ifdef CONFIG_AUDIOUTILS_PLAYLIST
  #define PLAYLISTFILE_PATH  "/mnt/sd0/PLAYLIST" /* Path of playlist file. */
  #define PLAY_LIST_NAME     "TRACK_DB.CSV" /* PlayList file name. */
#endif
#define PLAYER_DEF_VOLUME -200 //default volume -200
#define PLAYER_PLAY_TIME 10 //default play time
#define PLAYER_PLAY_FILE_NUM 5 //file number
#define FIFO_FRAME_SIZE  3840 // fifo set to 24bit WAW
#define FIFO_ELEMENT_NUM  10 //how many elements are used in the fifo qeue
#define PLAYER_FIFO_PUSH_NUM_MAX  5 //push limit to fifo
//info for when not using playlist
#define PLAYBACK_FILE_NAME     "Sound.mp3"
#define PLAYBACK_CH_NUM        AS_CHANNEL_STEREO
#define PLAYBACK_BIT_LEN       AS_BITLENGTH_16
#define PLAYBACK_SAMPLING_RATE AS_SAMPLINGRATE_48000   
#define PLAYBACK_CODEC_TYPE    AS_CODECTYPE_MP3


//Settings defined by the Config
#ifdef CONFIG_EXAMPLES_AUDIO_PLAYER_OUTPUT_DEV_SPHP
#  define PLAYER_OUTPUT_DEV AS_SETPLAYER_OUTPUTDEVICE_SPHP
#  define PLAYER_MIXER_OUT  HPOutputDevice
#else
#  define PLAYER_OUTPUT_DEV AS_SETPLAYER_OUTPUTDEVICE_I2SOUTPUT
#  define PLAYER_MIXER_OUT  I2SOutputDevice
#endif

// Definition depending on player mode
#ifdef CONFIG_EXAMPLES_AUDIO_PLAYER_MODE_HIRES
#  define FIFO_FRAME_NUM  4
#else
#  define FIFO_FRAME_NUM  1
#endif

//FIFO Config
#define FIFO_ELEMENT_SIZE  (FIFO_FRAME_SIZE * FIFO_FRAME_NUM)
#define FIFO_QUEUE_SIZE    (FIFO_ELEMENT_SIZE * FIFO_ELEMENT_NUM)
//Local FIFO error codes
#define FIFO_RESULT_OK  0
#define FIFO_RESULT_ERR 1
#define FIFO_RESULT_EOF 2
#define FIFO_RESULT_FUL 3


//Structs used by program
struct player_fifo_info_s //For FIFO
{
  CMN_SimpleFifoHandle          handle;
  AsPlayerInputDeviceHdlrForRAM input_device;
  uint32_t fifo_area[FIFO_QUEUE_SIZE/sizeof(uint32_t)];
  uint8_t  read_buf[FIFO_ELEMENT_SIZE];
};

#ifndef CONFIG_AUDIOUTILS_PLAYLIST
struct Track
{
  char title[64];
  uint8_t   channel_number;  /* Channel number. */
  uint8_t   bit_length;      /* Bit length.     */
  uint32_t  sampling_rate;   /* Sampling rate.  */
  uint8_t   codec_type;      /* Codec type.     */
};
#endif

struct player_file_info_s // For play file
{
  Track   track;
  int32_t size;
  DIR    *dirp;
  int     fd;
};

struct player_info_s // Player info
{
  struct player_fifo_info_s   fifo;
  struct player_file_info_s   file;
#ifdef CONFIG_AUDIOUTILS_PLAYLIST
  Playlist *playlist_ins = NULL;
#endif
};

//defining structs
static struct player_info_s  s_player_info; //contolling player
static mpshm_t s_shm; //Shared memory
static struct pm_cpu_freqlock_s s_player_lock; //frequency lock



/////////////////////////////////////////////////////////////////////////
static void outmixer_send_callback(int32_t identifier, bool is_end)
{
  AsRequestNextParam next;

  next.type = (!is_end) ? AsNextNormalRequest : AsNextStopResRequest;

  AS_RequestNextPlayerProcess(AS_PLAYER_ID_0, &next);

  return;
}

static void player_decode_done_callback(AsPcmDataParam pcm)
{
  AsSendDataOutputMixer data;

  data.handle   = OutputMixer0;
  data.callback = outmixer_send_callback;
  data.pcm      = pcm;

  /* You can imprement any audio signal process */

  AS_SendDataOutputMixer(&data);
}

static void app_init_freq_lock(void)
{
  s_player_lock.count = 0;
  s_player_lock.info = PM_CPUFREQLOCK_TAG('A', 'P', 0);
  s_player_lock.flag = PM_CPUFREQLOCK_FLAG_HV;
}

static void app_freq_lock(void)
{
  up_pm_acquire_freqlock(&s_player_lock);
}

static void app_freq_release(void)
{
  up_pm_release_freqlock(&s_player_lock);
}

static bool app_open_contents_dir(void)
{
  DIR *dirp;
  const char *name = PLAYBACK_FILE_PATH;
  
  dirp = opendir(name);

  if (!dirp)
    {
      printf("Error: %s directory path error. check the path!\n", name);
      return false;
    }

  s_player_info.file.dirp = dirp;

  return true;
}

static bool app_close_contents_dir(void)
{
  closedir(s_player_info.file.dirp);

  return true;
}

#ifdef CONFIG_AUDIOUTILS_PLAYLIST
static bool app_open_playlist(void)
{
  bool result = false;

  if (s_player_info.playlist_ins != NULL)
    {
      printf("Error: Open playlist failure. Playlist is already open\n");
      return false;
    }

  s_player_info.playlist_ins = new Playlist(PLAY_LIST_NAME);
  
  result = s_player_info.playlist_ins->init(PLAYLISTFILE_PATH);
  if (!result)
    {
      printf("Error: Playlist::init() failure.\n");
      return false;
    }

  s_player_info.playlist_ins->setPlayMode(Playlist::PlayModeNormal);
  if (!result)
    {
      printf("Error: Playlist::setPlayMode() failure.\n");
      return false;
    }

  s_player_info.playlist_ins->setRepeatMode(Playlist::RepeatModeOff);
  if (!result)
    {
      printf("Error: Playlist::setRepeatMode() failure.\n");
      return false;
    }

  s_player_info.playlist_ins->select(Playlist::ListTypeAllTrack, NULL);
  if (!result)
    {
      printf("Error: Playlist::select() failure.\n");
      return false;
    }

  return true;
}

static bool app_close_playlist(void)
{
  if (s_player_info.playlist_ins == NULL)
    {
      printf("Error: Close playlist failure. Playlist is not open\n");
      return false;
    }

  delete s_player_info.playlist_ins;
  s_player_info.playlist_ins = NULL;

  return true;
}
#endif /* #ifdef CONFIG_AUDIOUTILS_PLAYLIST */

static bool app_get_next_track(Track* track)
{
  bool ret;

#ifdef CONFIG_AUDIOUTILS_PLAYLIST
  if (s_player_info.playlist_ins == NULL)
  {
      printf("Error: Get next track failure. Playlist is not open\n");
      return false;
    }

  ret = s_player_info.playlist_ins->getNextTrack(track);
#else
  printf("inside app_get_next_track\n");
  snprintf(track->title, sizeof(track->title), "%s", PLAYBACK_FILE_NAME);
  track->channel_number = PLAYBACK_CH_NUM;
  track->bit_length     = PLAYBACK_BIT_LEN;
  track->sampling_rate  = PLAYBACK_SAMPLING_RATE;
  track->codec_type     = PLAYBACK_CODEC_TYPE;
#endif /* #ifdef CONFIG_AUDIOUTILS_PLAYLIST */
  return ret;
}

static void app_input_device_callback(uint32_t size)
{
    /* do nothing */
}

static bool app_init_simple_fifo(void)
{
  if (CMN_SimpleFifoInitialize(&s_player_info.fifo.handle,
                               s_player_info.fifo.fifo_area,
                               FIFO_QUEUE_SIZE,
                               NULL) != 0)
    {
      printf("Error: Fail to initialize simple FIFO.");
      return false;
    }
  CMN_SimpleFifoClear(&s_player_info.fifo.handle);

  s_player_info.fifo.input_device.simple_fifo_handler = (void*)(&s_player_info.fifo.handle);
  s_player_info.fifo.input_device.callback_function = app_input_device_callback;

  return true;
}

static int app_push_simple_fifo(int fd)
{
  int ret;

  ret = read(fd, &s_player_info.fifo.read_buf, FIFO_ELEMENT_SIZE);
  if (ret < 0)
    {
      printf("Error: Fail to read file. errno:%d\n", get_errno());
      return FIFO_RESULT_ERR;
    }

  if (CMN_SimpleFifoOffer(&s_player_info.fifo.handle, (const void*)(s_player_info.fifo.read_buf), ret) == 0)
    {
      return FIFO_RESULT_FUL;
    }
  s_player_info.file.size = (s_player_info.file.size - ret);
  if (s_player_info.file.size == 0)
    {
      return FIFO_RESULT_EOF;
    }
  return FIFO_RESULT_OK;
}

static bool app_first_push_simple_fifo(int fd)
{
  int i;
  int ret = 0;

  for(i = 0; i < FIFO_ELEMENT_NUM - 1; i++)
    {
      if ((ret = app_push_simple_fifo(fd)) != FIFO_RESULT_OK)
        {
          break;
        }
    }

  return (ret != FIFO_RESULT_ERR) ? true : false;
}

static bool app_refill_simple_fifo(int fd)
{
  int32_t ret = FIFO_RESULT_OK;
  size_t  vacant_size;

  vacant_size = CMN_SimpleFifoGetVacantSize(&s_player_info.fifo.handle);

  if ((vacant_size != 0) && (vacant_size > FIFO_ELEMENT_SIZE))
    {
      int push_cnt = vacant_size / FIFO_ELEMENT_SIZE;

      push_cnt = (push_cnt >= PLAYER_FIFO_PUSH_NUM_MAX) ?
                  PLAYER_FIFO_PUSH_NUM_MAX : push_cnt;

      for (int i = 0; i < push_cnt; i++)
        {
          if ((ret = app_push_simple_fifo(fd)) != FIFO_RESULT_OK)
            {
              break;
            }
        }
    }

  return (ret == FIFO_RESULT_OK) ? true : false;
}

static void app_attention_callback(const ErrorAttentionParam *attparam)
{
  printf("Attention!! %s L%d ecode %d subcode %ld\n",
          attparam->error_filename,
          attparam->line_number,
          attparam->error_code,
          attparam->error_att_sub_code);
}

static bool app_create_audio_sub_system(void)
{
  bool result = false;

  AsCreatePlayerParams_t player_create_param;
  player_create_param.msgq_id.player = MSGQ_AUD_PLY0;
  player_create_param.msgq_id.mng    = MSGQ_AUD_MNG;
  player_create_param.msgq_id.mixer  = MSGQ_AUD_OUTPUT_MIX;
  player_create_param.msgq_id.dsp    = MSGQ_AUD_DSP;
  player_create_param.pool_id.es     = S0_DEC_ES_MAIN_BUF_POOL;
  player_create_param.pool_id.pcm    = S0_REND_PCM_BUF_POOL;
  player_create_param.pool_id.dsp    = S0_DEC_APU_CMD_POOL;
  player_create_param.pool_id.src_work = S0_SRC_WORK_BUF_POOL;

  result = AS_CreatePlayerMulti(AS_PLAYER_ID_0, &player_create_param, app_attention_callback);

  if (!result)
    {
      printf("Error: AS_CratePlayer() failure. system memory insufficient!\n");
      return false;
    }

  /* Create mixer feature. */

  AsCreateOutputMixParams_t output_mix_act_param;
  output_mix_act_param.msgq_id.mixer = MSGQ_AUD_OUTPUT_MIX;
  output_mix_act_param.msgq_id.mng   = MSGQ_AUD_MNG;
  output_mix_act_param.msgq_id.render_path0_filter_dsp = MSGQ_AUD_PFDSP0;
  output_mix_act_param.msgq_id.render_path1_filter_dsp = MSGQ_AUD_PFDSP1;
  output_mix_act_param.pool_id.render_path0_filter_pcm = S0_PF0_PCM_BUF_POOL;
  output_mix_act_param.pool_id.render_path1_filter_pcm = S0_PF1_PCM_BUF_POOL;
  output_mix_act_param.pool_id.render_path0_filter_dsp = S0_PF0_APU_CMD_POOL;
  output_mix_act_param.pool_id.render_path1_filter_dsp = S0_PF1_APU_CMD_POOL;

  result = AS_CreateOutputMixer(&output_mix_act_param, app_attention_callback);
  if (!result)
    {
      printf("Error: AS_CreateOutputMixer() failed. system memory insufficient!\n");
      return false;
    }

  /* Create renderer feature. */

  AsCreateRendererParam_t renderer_create_param;
  renderer_create_param.msgq_id.dev0_req  = MSGQ_AUD_RND_PLY0;
  renderer_create_param.msgq_id.dev0_sync = MSGQ_AUD_RND_PLY0_SYNC;
  renderer_create_param.msgq_id.dev1_req  = 0xFF;
  renderer_create_param.msgq_id.dev1_sync = 0xFF;

  result = AS_CreateRenderer(&renderer_create_param);
  if (!result)
    {
      printf("Error: AS_CreateRenderer() failure. system memory insufficient!\n");
      return false;
    }

  return true;
}

static void app_deact_audio_sub_system(void)
{
  AS_DeletePlayer(AS_PLAYER_ID_0);
  AS_DeleteOutputMix();
  AS_DeleteRenderer();
}

static bool app_receive_object_reply(uint32_t id)
{
  AudioObjReply reply_info;
  AS_ReceiveObjectReply(MSGQ_AUD_MNG, &reply_info);

  if (reply_info.type != AS_OBJ_REPLY_TYPE_REQ)
    {
      printf("app_receive_object_reply() error! type 0x%x\n",
             reply_info.type);
      return false;
    }

  if (reply_info.id != id)
    {
      printf("app_receive_object_reply() error! id 0x%lx(request id 0x%lx)\n",
             reply_info.id, id);
      return false;
    }

  if (reply_info.result != AS_ECODE_OK)
    {
      printf("app_receive_object_reply() error! result 0x%lx\n",
             reply_info.result);
      return false;
    }

  return true;
}

static bool app_activate_baseband(void)
{
  CXD56_AUDIO_ECODE error_code;

  /* Power on audio device */

  error_code = cxd56_audio_poweron();

  if (error_code != CXD56_AUDIO_ECODE_OK)
    {
      printf("cxd56_audio_poweron() error! [%d]\n", error_code);
      return false;
    }

  /* Activate OutputMixer */

  AsActivateOutputMixer mixer_act;

  mixer_act.output_device = PLAYER_MIXER_OUT;
  mixer_act.mixer_type    = MainOnly;
  mixer_act.post_enable   = PostFilterDisable;
  mixer_act.cb            = NULL;

  AS_ActivateOutputMixer(OutputMixer0, &mixer_act);

  if (!app_receive_object_reply(MSG_AUD_MIX_CMD_ACT))
    {
      printf("AS_ActivateOutputMixer() error!\n");
    }

  return true;
}

static bool app_deactivate_baseband(void)
{
  /* Deactivate OutputMixer */

  AsDeactivateOutputMixer mixer_deact;

  AS_DeactivateOutputMixer(OutputMixer0, &mixer_deact);

  if (!app_receive_object_reply(MSG_AUD_MIX_CMD_DEACT))
    {
      printf("AS_DeactivateOutputMixer() error!\n");
    }

  CXD56_AUDIO_ECODE error_code;

  /* Power off audio device */

  error_code = cxd56_audio_poweroff();

  if (error_code != CXD56_AUDIO_ECODE_OK)
    {
      printf("cxd56_audio_poweroff() error! [%d]\n", error_code);
      return false;
    }

  return true;
}

static bool app_set_volume(int master_db)
{
  /* Set volume to audio driver */

  CXD56_AUDIO_ECODE error_code;

  error_code = cxd56_audio_set_vol(CXD56_AUDIO_VOLID_MIXER_OUT, master_db);

  if (error_code != CXD56_AUDIO_ECODE_OK)
    {
      printf("cxd56_audio_set_vol() error! [%d]\n", error_code);
      return false;
    }

  error_code = cxd56_audio_set_vol(CXD56_AUDIO_VOLID_MIXER_IN1, 0);

  if (error_code != CXD56_AUDIO_ECODE_OK)
    {
      printf("cxd56_audio_set_vol() error! [%d]\n", error_code);
      return false;
    }

  error_code = cxd56_audio_set_vol(CXD56_AUDIO_VOLID_MIXER_IN2, 0);

  if (error_code != CXD56_AUDIO_ECODE_OK)
    {
      printf("cxd56_audio_set_vol() error! [%d]\n", error_code);
      return false;
    }

  return true;
}

static bool app_activate_player_system(void)
{
  /* Activate MediaPlayer */

  AsActivatePlayer player_act;

  player_act.param.input_device  = AS_SETPLAYER_INPUTDEVICE_RAM;
  player_act.param.ram_handler   = &s_player_info.fifo.input_device;
  player_act.param.output_device = PLAYER_OUTPUT_DEV;
  player_act.cb                  = NULL;

  /* If manager set NULL to callback function at activate function,
   * a response is sent to MessageQueueID of manager specified
   * at create function.
   * Wait until there is a response of the type specified in the argument.
   */

  AS_ActivatePlayer(AS_PLAYER_ID_0, &player_act);

  if (!app_receive_object_reply(MSG_AUD_PLY_CMD_ACT))
    {
      printf("AS_ActivatePlayer() error!\n");
    }

  return true;
}

static bool app_init_player(uint8_t codec_type,
                            uint32_t sampling_rate,
                            uint8_t channel_number,
                            uint8_t bit_length) {
  AsInitPlayerParam player_init;

  player_init.codec_type     = codec_type;
  player_init.bit_length     = bit_length;
  player_init.channel_number = channel_number;
  player_init.sampling_rate  = sampling_rate;
  snprintf(player_init.dsp_path, AS_AUDIO_DSP_PATH_LEN, "%s", DSPBIN_FILE_PATH);
 
  AS_InitPlayer(AS_PLAYER_ID_0, &player_init);

  if (!app_receive_object_reply(MSG_AUD_PLY_CMD_INIT))
    {
      printf("AS_InitPlayer() error!\n");
    }

  return true;
}

static bool app_init_outputmixer(void)
{
  AsInitOutputMixer omix_init;

#ifdef CONFIG_EXAMPLES_AUDIO_PLAYER_OBJIF_USEPOSTPROC
  omix_init.postproc_type = AsPostprocTypeUserCustom;
#else /* CONFIG_EXAMPLES_AUDIO_PLAYER_OBJIF_USEPOSTPROC */
  omix_init.postproc_type = AsPostprocTypeThrough;
#endif /* CONFIG_EXAMPLES_AUDIO_PLAYER_OBJIF_USEPOSTPROC */
  snprintf(omix_init.dsp_path, sizeof(omix_init.dsp_path), "%s/POSTPROC", DSPBIN_FILE_PATH);

  AS_InitOutputMixer(OutputMixer0, &omix_init);

  if (!app_receive_object_reply(MSG_AUD_MIX_CMD_INIT))
    {
      printf("AS_InitOutputMixer() error!\n");
      return false;
    }

  return true;
}

#ifdef CONFIG_EXAMPLES_AUDIO_PLAYER_OBJIF_USEPOSTPROC
static bool app_init_postprocess_dsp()
{
  AsInitPostProc param;
  InitParam initpostcmd;

  param.addr = reinterpret_cast<uint8_t *>(&initpostcmd);
  param.size = sizeof(initpostcmd);
  
  AS_InitPostprocOutputMixer(OutputMixer0, &param);

  if (!app_receive_object_reply(MSG_AUD_MIX_CMD_INITMPP))
    {
      printf("AS_InitPostprocOutputMixer() error!\n");
      return false;
    }

  return true;
}

static bool app_set_postprocess_dsp(void)
{
  AsSetPostProc param;
  static bool s_toggle = false;
  s_toggle = (s_toggle) ? false : true;

  /* Create packet area (have to ensure until API returns.)  */

  SetParam setpostcmd;
  setpostcmd.enable = s_toggle;
  setpostcmd.coef = 99;

  param.addr = reinterpret_cast<uint8_t *>(&setpostcmd);
  param.size = sizeof(SetParam);

  AS_SetPostprocOutputMixer(OutputMixer0, &param);

  if (!app_receive_object_reply(MSG_AUD_MIX_CMD_SETMPP))
    {
      printf("AS_SetPostprocOutputMixer() error!\n");
      return false;
    }
 
  return true;
}
#endif /* CONFIG_EXAMPLES_AUDIO_PLAYER_OBJIF_USEPOSTPROC */

static bool app_play_player(void)
{
  AsPlayPlayerParam player_play;

  player_play.pcm_path          = AsPcmDataReply;
  player_play.pcm_dest.callback = player_decode_done_callback;

  AS_PlayPlayer(AS_PLAYER_ID_0, &player_play);

  if (!app_receive_object_reply(MSG_AUD_PLY_CMD_PLAY))
    {
      printf("AS_PlayPlayer() error!\n");
    }

  return true;
}

static bool app_stop_player(int mode)
{
  AsStopPlayerParam player_stop; 

  player_stop.stop_mode = mode;

  AS_StopPlayer(AS_PLAYER_ID_0, &player_stop);

  if (!app_receive_object_reply(MSG_AUD_PLY_CMD_STOP))
    {
      printf("AS_StopPlayer() error!\n");
    }

  return true;
}

static bool app_set_clkmode(int clk_mode)
{
  CXD56_AUDIO_ECODE error_code = CXD56_AUDIO_ECODE_OK;

  cxd56_audio_clkmode_t mode;

  mode = (clk_mode == AS_CLKMODE_NORMAL)
           ? CXD56_AUDIO_CLKMODE_NORMAL : CXD56_AUDIO_CLKMODE_HIRES;

  error_code = cxd56_audio_set_clkmode(mode);

  if (error_code != CXD56_AUDIO_ECODE_OK)
    {
      printf("cxd56_audio_set_clkmode() error! [%d]\n", error_code);
      return false;
    }

  return true;
}

static bool app_deact_player_system(void)
{
  /* Deactivate MediaPlayer */

  AsDeactivatePlayer player_deact;

  AS_DeactivatePlayer(AS_PLAYER_ID_0, &player_deact);

  if (!app_receive_object_reply(MSG_AUD_PLY_CMD_DEACT))
    {
      printf("AS_DeactivatePlayer() error!\n");
    }

  return true;
}

static bool app_init_libraries(void)
{
  int ret;
  uint32_t addr = AUD_SRAM_ADDR;

  /* Initialize shared memory.*/

  ret = mpshm_init(&s_shm, 1, 1024 * 128 * 2);
  if (ret < 0)
    {
      printf("Error: mpshm_init() failure. %d\n", ret);
      return false;
    }

  ret = mpshm_remap(&s_shm, (void *)addr);
  if (ret < 0)
    {
      printf("Error: mpshm_remap() failure. %d\n", ret);
      return false;
    }

  /* Initalize MessageLib. */

  err_t err = MsgLib::initFirst(NUM_MSGQ_POOLS, MSGQ_TOP_DRM);
  if (err != ERR_OK)
    {
      printf("Error: MsgLib::initFirst() failure. 0x%x\n", err);
      return false;
    }

  err = MsgLib::initPerCpu();
  if (err != ERR_OK)
    {
      printf("Error: MsgLib::initPerCpu() failure. 0x%x\n", err);
      return false;
    }

  void* mml_data_area = translatePoolAddrToVa(MEMMGR_DATA_AREA_ADDR);
  err = Manager::initFirst(mml_data_area, MEMMGR_DATA_AREA_SIZE);
  if (err != ERR_OK)
    {
      printf("Error: Manager::initFirst() failure. 0x%x\n", err);
      return false;
    }

  err = Manager::initPerCpu(mml_data_area, static_pools, pool_num, layout_no);
  if (err != ERR_OK)
    {
      printf("Error: Manager::initPerCpu() failure. 0x%x\n", err);
      return false;
    }

  /* Create static memory pool of AudioPlayer. */

  const uint8_t sec_no = AUDIO_SECTION;
  const NumLayout layout_no = MEM_LAYOUT_PLAYER_MAIN_ONLY;
  void* work_va = translatePoolAddrToVa(S0_MEMMGR_WORK_AREA_ADDR);
  const PoolSectionAttr *ptr  = &MemoryPoolLayouts[AUDIO_SECTION][layout_no][0];
  err = Manager::createStaticPools(sec_no, layout_no,
                             work_va,
                             S0_MEMMGR_WORK_AREA_SIZE,
                             ptr);
  if (err != ERR_OK)
    {
      printf("Error: Manager::createStaticPools() failure. %d\n", err);
      return false;
    }

  return true;
}

static bool app_finalize_libraries(void)
{
  /* Finalize MessageLib. */

  MsgLib::finalize();

  /* Destroy static pools. */

  MemMgrLite::Manager::destroyStaticPools(AUDIO_SECTION);

  /* Finalize memory manager. */

  MemMgrLite::Manager::finalize();

  /* Destroy shared memory. */

  int ret;
  ret = mpshm_detach(&s_shm);
  if (ret < 0)
    {
      printf("Error: mpshm_detach() failure. %d\n", ret);
      return false;
    }

  ret = mpshm_destroy(&s_shm);
  if (ret < 0)
    {
      printf("Error: mpshm_destroy() failure. %d\n", ret);
      return false;
    }

  return true;
}

static int app_play_file_open(FAR const char *file_path, FAR int32_t *file_size)
{
  int fd = open(file_path, O_RDONLY);

  *file_size = 0;
  if (fd >= 0)
    {
      struct stat stat_buf;
      if (stat(file_path, &stat_buf) == OK)
        {
          *file_size = stat_buf.st_size;
        }
    }

  return fd;
}

static bool app_open_next_play_file(void)
{
  /* Get next track */
  printf("attempting to gater tracks\n");
  if (!app_get_next_track(&s_player_info.file.track))
    {
      printf("Error: No more tracks to play.\n");
      return false;
    }

  char full_path[128];
  snprintf(full_path,
           sizeof(full_path),
           "%s/%s",
           PLAYBACK_FILE_PATH,
           s_player_info.file.track.title);

  s_player_info.file.fd = app_play_file_open(full_path, &s_player_info.file.size);
  if (s_player_info.file.fd < 0)
    {
      printf("Error: %s open error. check paths and files!\n", full_path);
      return false;
    }
  if (s_player_info.file.size == 0)
    {
      close(s_player_info.file.fd);
      printf("Error: %s file size is abnormal. check files!\n",full_path);
      return false;
    }

  /* Push data to simple fifo */

  if (!app_first_push_simple_fifo(s_player_info.file.fd))
    {
      printf("Error: app_first_push_simple_fifo() failure.\n");
      CMN_SimpleFifoClear(&s_player_info.fifo.handle);
      close(s_player_info.file.fd);
      return false;
    }

  return true;
}

static bool app_close_play_file(void)
{
  if (close(s_player_info.file.fd) != 0)
    {
      printf("Error: close() failure.\n");
      return false;
    }

  CMN_SimpleFifoClear(&s_player_info.fifo.handle);

  return true;
}

static bool app_start(void)
{
  /* Init Player */

  Track *t = &s_player_info.file.track;
  if (!app_init_player(t->codec_type,
                       t->sampling_rate,
                       t->channel_number,
                       t->bit_length))
    {
      printf("Error: app_init_player() failure.\n");
      app_close_play_file();
      return false;
    }

  if (!app_init_outputmixer())
    {
      printf("Error: app_init_outputmixer() failure.\n");
      app_close_play_file();
      return false;
    }

#ifdef CONFIG_EXAMPLES_AUDIO_PLAYER_OBJIF_USEPOSTPROC
  if (!app_init_postprocess_dsp())
    {
      printf("Error: app_init_postprocess_dsp() failure.\n");
      app_close_play_file();
      return false;
    }
#endif /* CONFIG_EXAMPLES_AUDIO_PLAYER_OBJIF_USEPOSTPROC */

  if (!app_play_player())
    {
      printf("Error: app_play_player() failure.\n");
      CMN_SimpleFifoClear(&s_player_info.fifo.handle);
      close(s_player_info.file.fd);
      app_freq_release();
      return false;
    }

  return true;
}

static bool app_stop(void)
{
  bool result = true;

  /* Set stop mode.
   * If the end of the file is detected, play back until the data empty
   * and then stop.(select AS_STOPPLAYER_ESEND)
   * Otherwise, stop immediate reproduction.(select AS_STOPPLAYER_NORMAL)
   */

  int  stop_mode = (s_player_info.file.size != 0) ?
                    AS_STOPPLAYER_NORMAL : AS_STOPPLAYER_ESEND;

  if (!app_stop_player(stop_mode))
    {
      printf("Error: app_stop_player() failure.\n");
      result = false;
    }

  if (!app_close_play_file())
    {
      printf("Error: close() failure.\n");
      result = false;
    }

  return result;
}

void app_play_process(uint32_t play_time)
{
  /* Timer Start */
  time_t start_time;
  time_t cur_time;

  time(&start_time);

  do
    {
      /* Check the FIFO every 2 ms and fill if there is space. */

      usleep(2 * 1000);
      if (!app_refill_simple_fifo(s_player_info.file.fd))
        {
          break;
        }

#ifdef CONFIG_EXAMPLES_AUDIO_PLAYER_OBJIF_USEPOSTPROC
      static int cnt = 0;
      if (cnt++ > 100)
        {
          app_set_postprocess_dsp();
          cnt = 0;
        }
#endif /* CONFIG_EXAMPLES_AUDIO_PLAYER_OBJIF_USEPOSTPROC */

    } while((time(&cur_time) - start_time) < play_time);
}
/////////////////////////////////////////////////////////////////////////

