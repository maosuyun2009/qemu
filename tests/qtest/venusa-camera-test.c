/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qapi/error.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
static uint32_t i2c_base = 0x43c00000;
#define I2C i2c_base
#define DVP 0x42000800
#define FIFO 0x42001000
static char *flash, *scene;
static uint64_t clock_denominator(QTestState *q)
{
    g_autoptr(QDict) result = qtest_qmp(q, "{'execute':'qom-get',"
        "'arguments':{'path':'/machine/peripheral/camera',"
        "'property':'x-lisem-camera-timing'}}");
    QObject *data = qobject_from_json(qdict_get_str(result, "return"), &error_abort);
    uint64_t value = qdict_get_int(qobject_to(QDict, data), "clock_denominator");
    qobject_unref(data);
    return value;
}
static void load_pulses(void)
{
    g_autofree char *f = g_shell_quote(flash), *p = g_shell_quote(scene);
    QTestState *q = qtest_initf("-machine venusa-rd-evb -drive "
        "if=pflash,format=raw,file=%s -device venusa-gc0328-camera,id=camera,"
        "soc=/machine/soc,file=%s", f, p);
    for (unsigned i = 21; i <= 31; i++) { qtest_writel(q, 0x44100000 + i * 4, 13); }
    qtest_writel(q, 0x44100000 + 35 * 4, 13);
    qtest_writel(q, 0x43100028, 0x30000 | (24 << 7));
    g_assert_cmphex(qtest_readl(q, 0x43100028), ==, 0x20000 | (24 << 7));
    g_assert_cmpuint(clock_denominator(q), ==, 24);
    /* Consecutive W1P writes each latch, without a software zero write. */
    qtest_writel(q, 0x43100028, 0x30000 | (12 << 7));
    g_assert_cmpuint(clock_denominator(q), ==, 12);
    qtest_writel(q, 0x43100028, 0x20000 | (6 << 7));
    g_assert_cmpuint(clock_denominator(q), ==, 12);
    qtest_writel(q, 0x43100028, 0x30000 | (6 << 7) | 0x23);
    g_assert_cmphex(qtest_readl(q, 0x43100028), ==, 0x20000 | (6 << 7) | 3);
    g_assert_cmpuint(clock_denominator(q), ==, 6);
    qtest_quit(q);
}
static void reg(QTestState *q, uint8_t address, uint8_t value)
{
    qtest_writel(q,I2C+0x28,5); qtest_writel(q,I2C+0x1c,0x21);
    qtest_writel(q,I2C+0x24,0x1e02);
    qtest_writel(q,I2C+0x20,address);qtest_writel(q,I2C+0x20,value);
    qtest_writel(q,I2C+0x28,1);qtest_clock_step(q,1000000);
    g_assert_cmphex(qtest_readl(q,I2C+0x18)&0x600,==,0x600);
}
static void pixels(const void *opaque)
{
    unsigned bus=GPOINTER_TO_UINT(opaque);
    i2c_base=0x43b00000+bus*0x100000;
    g_autofree char *f=g_shell_quote(flash),*p=g_shell_quote(scene);
    QTestState *q=qtest_initf("-machine venusa-rd-evb -drive if=pflash,format=raw,file=%s -device venusa-gc0328-camera,soc=/machine/soc,file=%s,i2c-bus=%u",f,p,bus);
    qtest_writel(q,0x43100034,0x800|(1u<<bus));
    qtest_writel(q,0x44100000+(bus ? 34 : 19)*4,bus ? 8 : 7);
    qtest_writel(q,0x44100000+(bus ? 33 : 20)*4,bus ? 8 : 7);
    for(unsigned i=21;i<=31;i++)qtest_writel(q,0x44100000+i*4,13);
    qtest_writel(q,0x44100000+35*4,13);
    qtest_writel(q,0x43100028,0x20000|(24<<7));
    qtest_writel(q,0x43100028,0x30000|(24<<7));
    qtest_writel(q,I2C+0x2c,5|(200<<4));
    reg(q,0xf2,0);reg(q,0x44,6);reg(q,0x46,2);
    reg(q,0x50,1);reg(q,0x55,0);reg(q,0x56,4);reg(q,0x57,0);reg(q,0x58,4);
    qtest_writel(q,DVP,4);qtest_writel(q,DVP+4,4);
    qtest_writel(q,DVP+0x10,8);qtest_writel(q,DVP+0x18,12);
    qtest_writel(q,DVP+0x1c,1);reg(q,0xf2,1);
    qtest_clock_step(q,17000000);
    g_assert_cmphex(qtest_readl(q,DVP+0x34)&3,==,0);
    qtest_clock_step(q,13000000);
    for(unsigned y=0;y<4;y++)for(unsigned x=0;x<4;x+=2){
        unsigned a=((x*40>>3)<<11)|((y*40>>2)<<5)|7;
        unsigned b=(((x+1)*40>>3)<<11)|((y*40>>2)<<5)|7;
        unsigned expected=(a>>8)|((a&255)<<8)|((b>>8)<<16)|((b&255)<<24);
        g_assert_cmphex(qtest_readl(q,FIFO),==,expected);
    }
    g_assert_cmphex(qtest_readl(q,DVP+0x34)&0x240,==,0x240);
    qtest_writel(q,DVP+0x28,0xfff);
    qtest_clock_step(q,900000000);
    g_assert_cmphex(qtest_readl(q,DVP+0x34)&3,==,0);
    qtest_clock_step(q,50000000);
    g_assert_cmphex(qtest_readl(q,DVP+0x34)&3,==,3);
    qtest_writel(q,0x43100028,0x10000|(24<<7));
    qtest_writel(q,DVP+0x14,1);qtest_clock_step(q,100000000);
    g_assert_cmphex(qtest_readl(q,DVP+0x34),==,0x10);
    qtest_quit(q);
}
static void post_subsample_crop(void)
{
    g_autofree char *f=g_shell_quote(flash), *p=g_shell_quote(scene);
    QTestState *q=qtest_initf("-machine venusa-rd-evb -drive if=pflash,format=raw,file=%s "
        "-device venusa-gc0328-camera,id=camera,soc=/machine/soc,file=%s",f,p);
    i2c_base=0x43c00000;
    qtest_writel(q,0x43100034,0x802);
    for (unsigned i=21;i<=31;i++) { qtest_writel(q,0x44100000+i*4,13); }
    qtest_writel(q,0x44100000+33*4,8); qtest_writel(q,0x44100000+34*4,8);
    qtest_writel(q,0x44100000+35*4,13);
    qtest_writel(q,0x43100028,0x30000|(24<<7));
    qtest_writel(q,I2C+0x2c,5|(200<<4));
    reg(q,0xf2,0); reg(q,0x50,1);
    reg(q,0x55,0); reg(q,0x56,240); reg(q,0x57,1); reg(q,0x58,64);
    reg(q,0x59,0x22); reg(q,0x5a,0x0e);
    for (unsigned i=0x5b;i<=0x62;i++) { reg(q,i,0); }
    reg(q,0xf2,1);
    g_autoptr(QDict) result=qtest_qmp(q,"{'execute':'qom-get','arguments':"
        "{'path':'/machine/peripheral/camera','property':'x-lisem-camera-timing'}}");
    QObject *obj=qobject_from_json(qdict_get_str(result,"return"),&error_abort);
    QDict *timing=qobject_to(QDict,obj);
    g_assert_cmpuint(qdict_get_int(timing,"width"),==,320);
    g_assert_cmpuint(qdict_get_int(timing,"height"),==,240);
    qobject_unref(obj); qtest_quit(q);
}
static void reject_unrouted_sccb(void)
{
    if (g_test_subprocess()) {
        g_autofree char *f=g_shell_quote(flash), *p=g_shell_quote(scene);
        QTestState *q=qtest_initf("-machine venusa-rd-evb -drive if=pflash,format=raw,file=%s "
            "-device venusa-gc0328-camera,soc=/machine/soc,file=%s",f,p);
        i2c_base=0x43c00000;
        qtest_writel(q,0x43100034,2);
        qtest_writel(q,0x44100000+35*4,13);
        qtest_writel(q,0x43100028,0x30000|(24<<7));
        qtest_writel(q,I2C+0x2c,5|(200<<4));
        reg(q,0xf2,0); qtest_quit(q); return;
    }
    g_test_trap_subprocess(NULL,5*G_USEC_PER_SEC,0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*VenusA unsupported write*");
}
int main(int argc,char**argv){
    g_test_init(&argc,&argv,NULL);
    int fd=g_file_open_tmp("venusa-camera-flash-XXXXXX",&flash,NULL);
    g_assert_cmpint(ftruncate(fd,16*1024*1024),==,0);close(fd);
    fd=g_file_open_tmp("venusa-camera-scene-XXXXXX",&scene,NULL);
    uint8_t *data=g_malloc(640*480*3);
    for(unsigned y=0;y<480;y++)for(unsigned x=0;x<640;x++){
        data[(y*640+x)*3]=x*40;data[(y*640+x)*3+1]=y*40;data[(y*640+x)*3+2]=56;
    }
    g_assert_cmpint(write(fd,data,640*480*3),==,640*480*3);close(fd);g_free(data);
    qtest_add_data_func("venusa/camera/sccb-pixels-clock",GUINT_TO_POINTER(1),pixels);
    qtest_add_data_func("venusa/camera/sccb0-pixels-clock",GUINT_TO_POINTER(0),pixels);
    qtest_add_func("venusa/camera/repeated-clock-load",load_pulses);
    qtest_add_func("venusa/camera/reject-unrouted-sccb",reject_unrouted_sccb);
    qtest_add_func("venusa/camera/post-subsample-crop",post_subsample_crop);
    int r=g_test_run();unlink(flash);unlink(scene);g_free(flash);g_free(scene);return r;
}
