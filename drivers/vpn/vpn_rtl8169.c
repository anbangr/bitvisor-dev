/*
 * Copyright (c) 2007, 2008 University of Tsukuba
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. Neither the name of the University of Tsukuba nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifdef VPN
#ifdef VPN_RTL8169

#include "pci.h"
#include "../../crypto/chelp.h"
#include <core/mmio.h>
#include <core/tty.h>
#include <core/vpnsys.h>
#include <core.h>
#include <Se/SeVpn.h>
#include <Se/SeKernel.h>
#include <Se/SeSec.h>
#include <core/time.h>
#include "vpn_rtl8169.h"

u16 ipchecksum (void *buf, u32 len);

static const char driver_name[]     = "vpn_rtl8169_driver";
static const char driver_longname[] = "VPN for RealTek RTL8169";

static struct pci_driver vpn_rtl8169_driver = {
	.name         = driver_name,
	.longname     = driver_longname,
	.id           = {0x816810EC,0xFFFEFFFF},
	.class        = {0, 0},
	.new          = rtl8169_new,
	.config_read  = rtl8169_config_read,
	.config_write = rtl8169_config_write,
};

static bool
sendenabled (RTL8169_CTX *ctx)
{
#ifdef TTY_RTL8169
	if (!config.vmm.driver.vpn.RTL8169 &&
	    config.vmm.tty_rtl8169) /* tty only */
		return !!(ctx->enableflag & 1);
#endif	/* TTY_RTL8169 */
	return ctx->enableflag == 3;
}

static bool
recvenabled (RTL8169_CTX *ctx)
{
#ifdef TTY_RTL8169
	if (!config.vmm.driver.vpn.RTL8169 &&
	    config.vmm.tty_rtl8169) /* tty only */
		return !!(ctx->enableflag & 2);
#endif	/* TTY_RTL8169 */
	return ctx->enableflag == 3;
}

static void
GetPhysicalNicInfo (SE_HANDLE nic_handle, SE_NICINFO *info)
{
	RTL8169_CTX *ctx;

#ifdef _DEBUG
	time = get_cpu_time(); 
	printf("(%llu) ", time);
	printf("GetPhysicalNicInfo:start!\n");
#endif	

	if(info == NULL)
	{
#ifdef _DEBUG
		time = get_cpu_time(); 
		printf("(%llu) ", time);
		printf("GetPhysicalNicInfo:Error Parameter Invalid!\n");
#endif	
	}
	else
	{
		ctx = (RTL8169_CTX *)nic_handle;

		info->MediaType  = SE_MEDIA_TYPE_ETHERNET;
		info->Mtu        = 1500;
		info->MediaSpeed = 1000000000;

		memcpy (info->MacAddress, ctx->macaddr, RTL8169_MAC_LEN);

#ifdef _DEBUG
		time = get_cpu_time(); 
		printf("(%llu) ", time);
		printf("GetPhysicalNicInfo:end!\n");
#endif		
	}
}


static void
SendPhysicalNic (SE_HANDLE nic_handle, UINT num_packets, void **packets, UINT *packet_sizes)
{
	UINT		       i, ii;
	void		       *data;
	UINT		       size;
	struct desc	  	*TNPDSVirtAddr;
	struct desc	  	*TargetDesc;
	u8	  		npq;
	RTL8169_CTX		*ctx  = (RTL8169_CTX *)nic_handle;
	RTL8169_SUB_CTX	*sctx = ctx->sctx_mmio;

#ifdef _DEBUG
	time = get_cpu_time(); 
	printf("(%llu) ", time);
	printf("SendPhysicalNic:start!\n");
#endif
	//引数チェック
	if(packets == NULL || packet_sizes == NULL)
	{
#ifdef _DEBUG
		time = get_cpu_time(); 
		printf("(%llu) ", time);
		printf("SendPhysicalNic:Error Parameter Invalid!\n");
#endif
		return;
	}
	if (!sendenabled (ctx))
		return;

	TNPDSVirtAddr = ctx->TNPDSvirt;
	ii = 0;
	i = ctx->sendindex;
	while (ii < num_packets) {
		TargetDesc = &TNPDSVirtAddr[i];
		if (!(TargetDesc->opts & OPT_OWN)) {
			data       = packets[ii];
			size       = packet_sizes[ii];
			ii++;
			if (size >= 4096)
				continue;
			memcpy (ctx->tnbufvirt[i], data, size);
			TargetDesc->opts   &= OPT_EOR;
			TargetDesc->opts   |= size;
			TargetDesc->opts   |= OPT_FS;
			TargetDesc->opts   |= OPT_LS;
			TargetDesc->opts   |= OPT_OWN;
		}
		if (TargetDesc->opts & OPT_EOR)
			i = 0;
		else
			i++;
	}
	ctx->sendindex = i;

	// TpPollレジスタの通常送信ビットをONにし、送信を行う
	npq = RTL8169_REG_TPPOLL_NPQ;
	rtl8169_write(sctx, RTL8169_REG_TPPOLL, npq, 1);
	
#ifdef _DEBUG
	time = get_cpu_time(); 
	printf("(%llu) ", time);
	printf("SendPhysicalNic:end!\n");
#endif

	return;
}

//
// 物理NICへのパケット送信（送信処理）を行うコールバックを設定する
// ※本関数は、VPNクライアントからコールバックで呼び出される
//
static void
SetPhysicalNicRecvCallback (SE_HANDLE nic_handle, SE_SYS_CALLBACK_RECV_NIC *callback, void *param)
{
	RTL8169_CTX *ctx;

#ifdef _DEBUG
	time = get_cpu_time(); 
	printf("(%llu) ", time);
	printf("SetPhysicalNicRecvCallback:start!\n");
#endif

	if(callback == NULL || param == NULL)
	{
#ifdef _DEBUG
		time = get_cpu_time(); 
		printf("(%llu) ", time);
		printf("SetPhysicalNicRecvCallback:Error Parameter Invalid!\n");
#endif
		return;	
	}

	ctx				 = (RTL8169_CTX *)nic_handle;
	ctx->CallbackRecvPhyNic      = callback;
	ctx->CallbackRecvPhyNicParam = param;

	//Bitvisor MACアドレスの取得
	SE_ETH *Eth;
	Eth = (SE_ETH *)param;

#ifdef _DEBUG
	time = get_cpu_time(); 
	printf("(%llu) ", time);
	printf("SetPhysicalNicRecvCallback:end!\n");
#endif
	return;
}

static void
GetVirtualNicInfo (SE_HANDLE nic_handle, SE_NICINFO *info)
{
	RTL8169_CTX *ctx;

#ifdef _DEBUG
	time = get_cpu_time(); 
	printf("(%llu) ", time);
	printf("GetVirtualNicInfo:start!\n");
#endif

	if(info == NULL)
	{
#ifdef _DEBUG
		time = get_cpu_time(); 
		printf("(%llu) ", time);
		printf("GetVirtuallNicInfo:Error Parameter Invalid!\n");
#endif
	}
	else
	{
		ctx = (RTL8169_CTX *)nic_handle;

		info->MediaType  = SE_MEDIA_TYPE_ETHERNET;
		info->Mtu        = 1500;
		info->MediaSpeed = 1000000000;

		memcpy (info->MacAddress, ctx->macaddr, RTL8169_MAC_LEN);

#ifdef _DEBUG
		time = get_cpu_time(); 
		printf("(%llu) ", time);
		printf("GetVirtualNicInfo:end!\n");
#endif
	}
}

static void
makeintr (RTL8169_SUB_CTX *sctx)
{
	u8			TpPoll;

	TpPoll    =  rtl8169_read(sctx, RTL8169_REG_TPPOLL, 1);
	TpPoll    |= RTL8169_REG_TPPOLL_FSWINT;
	rtl8169_write(sctx, RTL8169_REG_TPPOLL, TpPoll, 1);
}

//
// 仮想NICにパケットを送信する（受信処理）
// ※本関数は、VPNクライアントからコールバックで呼び出される
//
static void
SendVirtualNic (SE_HANDLE nic_handle, UINT num_packets, void **packets, UINT *packet_sizes)
{
	UINT			i;
	void			*data;
	UINT			size;
	RTL8169_CTX		*ctx     = (RTL8169_CTX *)nic_handle;
	RTL8169_SUB_CTX	*sctx    = ctx->sctx_mmio;

#ifdef _DEBUG
	time = get_cpu_time(); 
	printf("(%llu) ", time);
	printf("SendVirtualNic:start!\n");
#endif

	//引数チェック
	if(packets == NULL || packet_sizes == NULL)
	{
#ifdef _DEBUG
		time = get_cpu_time(); 
		printf("(%llu) ", time);
		printf("SendVirtualNic:Error Parameter Invalid!\n");
#endif
		return;
	}
	if (!recvenabled (ctx))
		return;

	for(i = 0; i < num_packets; i++)
	{
		data = packets[i];
		size = packet_sizes[i];
		rtl8169_send_virt_nic(nic_handle, ctx->RDSARreg, data, size);
	}

	if (num_packets >= 1)
		makeintr (sctx);

#ifdef _DEBUG
	time = get_cpu_time(); 
	printf("(%llu) ", time);
	printf("SendVirtualNic:end!\n");
#endif

	return;
}
//
// 仮想NICにパケットを１つ送信する（受信処理）
//
static void 
rtl8169_send_virt_nic(SE_HANDLE nic_handle, phys_t rxdescphys, void *data, UINT size)
{
	RTL8169_CTX		*ctx         = (RTL8169_CTX *)nic_handle;
	int			m            = 0;
	void			*vptr        = NULL;
	struct desc		*TargetDesc;
	unsigned int		RxDescNum    = ctx->RxDescNum;

#ifdef _DEBUG
	time = get_cpu_time(); 
	printf("(%llu) ", time);
	printf("rtl8169_send_virt_nic:start!\n");
#endif

	//引数チェック	
	if(data == NULL)
	{
#ifdef _DEBUG
		time = get_cpu_time(); 
		printf("(%llu) ", time);
		printf("rtl8169_send_virt_nic:Error Parameter Invalid!\n");
#endif
		return;
	}


	for(m = 0; m < RTL8169_RXDESC_MAX_NUM; m++)
	{
		TargetDesc = mapmem_gphys (rxdescphys + sizeof *TargetDesc *
					   RxDescNum, sizeof *TargetDesc,
					   MAPMEM_WRITE);
		if (!TargetDesc) {
			printf ("mapmem err rxdescphys=0x%llX RxDescNum=%u\n",
				rxdescphys, RxDescNum);
			if (RxDescNum) {
				RxDescNum = 0;
				continue;
			}
			break;
		}
		//if(RxDescNum >= (RTL8169_RXDESC_MAX_NUM-1))
		if (TargetDesc->opts & OPT_EOR)
		{
			RxDescNum = 0;
		}
		else
		{
			RxDescNum++;
		}
		
		if(TargetDesc->opts & OPT_OWN)
		{
			vptr = mapmem_gphys (TargetDesc->addr, size, MAPMEM_WRITE | 0/*MAPMEM_PCD | MAPMEM_PWT*/ | 0/*MAPMEM_PAT*/);
			if (vptr)
			{
				TargetDesc->opts &= OPT_OWN | OPT_EOR;
				TargetDesc->opts |= size + 4;
				TargetDesc->opts |= OPT_FS;
				TargetDesc->opts |= OPT_LS;
				memcpy(vptr, data, size);
				TargetDesc->opts &= ~OPT_OWN;
				unmapmem(vptr, size);
				unmapmem (TargetDesc, sizeof *TargetDesc);
				break;
			}
		}
		unmapmem (TargetDesc, sizeof *TargetDesc);
	}

	ctx->RxDescNum = RxDescNum;

#ifdef _DEBUG
	time = get_cpu_time(); 
	printf("(%llu) ", time);
	printf("rtl8169_send_virt_nic:end!\n");
#endif
	return;
}
//
// 仮想NICへのパケット送信（受信処理）を行うコールバックを設定する
// ※本関数は、VPNクライアントからコールバックで呼び出される
//
static void
SetVirtualNicRecvCallback (SE_HANDLE nic_handle, SE_SYS_CALLBACK_RECV_NIC *callback, void *param)
{
	RTL8169_CTX *ctx;

#ifdef _DEBUG
	time = get_cpu_time(); 
	printf("(%llu) ", time);
	printf("SetVirtualNicRecvCallback:start!\n");
#endif

	if(callback == NULL || param == NULL)
	{
#ifdef _DEBUG
		time = get_cpu_time(); 
		printf("(%llu) ", time);
		printf("SetVirtualNicRecvCallback:Error Parameter Invalid!\n");
#endif
		return;
	}
	
	ctx                           = (RTL8169_CTX *)nic_handle;
	ctx->CallbackRecvVirtNic      = callback;
	ctx->CallbackRecvVirtNicParam = param;
	
#ifdef _DEBUG
	time = get_cpu_time(); 
	printf("(%llu) ", time);
	printf("SetVirtualNicRecvCallback:end!\n");
#endif
	return;
}

static struct nicfunc func = {
	.GetPhysicalNicInfo         = GetPhysicalNicInfo,
	.SendPhysicalNic            = SendPhysicalNic,
	.SetPhysicalNicRecvCallback = SetPhysicalNicRecvCallback,
	.GetVirtualNicInfo          = GetVirtualNicInfo,
	.SendVirtualNic             = SendVirtualNic,
	.SetVirtualNicRecvCallback  = SetVirtualNicRecvCallback,
};

//
//MACアドレスの取得
//
static bool
rtl8169_get_macaddr (struct RTL8169_SUB_CTX *sctx, void *buf)
{
	bool bret        = false;
	void *RegMacAddr = NULL;

#ifdef _DEBUG
	time = get_cpu_time(); 
	printf("(%llu) ", time);
	printf("rtl8169_get_macaddr:start!\n");
#endif

	if(sctx == NULL || buf == NULL)
	{
#ifdef _DEBUG
		time = get_cpu_time(); 
		printf("(%llu) ", time);
		printf("rtl8169_get_macaddr:Error Parameter Invalid!\n");
#endif
	}
	else
	{
		// レジスタの仮想アドレスをマップ
		//RegMacAddr = mapmem_gphys (sctx->mapaddr, RTL8169_MAC_LEN, MAPMEM_WRITE | MAPMEM_PCD | MAPMEM_PWT | 0/*MAPMEM_PAT*/);
		RegMacAddr = sctx->map;
		if(RegMacAddr == NULL)
		{
#ifdef _DEBUG
			time = get_cpu_time(); 
			printf("(%llu) ", time);
			printf("rtl8169_get_macaddr:Error Module mapmem_gphys!\n");
#endif
		}
		else
		{
			// レジスタからMACアドレスをコピー
			memcpy(buf, RegMacAddr, RTL8169_MAC_LEN);
			printf ("Mac Address is %02X:%02X:%02X:%02X:%02X:%02X\n", ((u8 *)buf)[0], ((u8 *)buf)[1], ((u8 *)buf)[2], ((u8 *)buf)[3], ((u8 *)buf)[4], ((u8 *)buf)[5]);
			// レジスタの仮想アドレスのマップ解除
			//unmapmem(RegMacAddr ,RTL8169_MAC_LEN);
			bret = true;
#ifdef _DEBUG
			time = get_cpu_time(); 
			printf("(%llu) ", time);
			printf("rtl8169_get_macaddr:end!\n");
#endif
		}
	}

	return bret;
}

static u32
getnum (u32 b)
{
	u32 ret;

	for (ret = 1; !(b & 1); b >>= 1)
		ret <<= 1;

	return ret;
}

static void
unreghook (struct RTL8169_SUB_CTX *sctx)
{

#ifdef _DEBUG
	time = get_cpu_time(); 
	printf("(%llu) ", time);
	printf("unreghook:start!\n");
#endif
	if(sctx == NULL)
	{
#ifdef _DEBUG
		time = get_cpu_time(); 
		printf("(%llu) ", time);
		printf("unreghook:Error Parameter Invalid!\n");
#endif
	}
	else
	{
		if (sctx->e) 
		{
			if (sctx->io)
			{
				core_io_unregister_handler (sctx->hd);
			}
			else
			{
				mmio_unregister (sctx->h);
				unmapmem (sctx->map, sctx->maplen);
			}
			sctx->e = 0;
#ifdef _DEBUG
			time = get_cpu_time(); 
			printf("(%llu) ", time);
			printf("unreghook:end!\n");
#endif
		}
#ifdef _DEBUG
		else
		{
			time = get_cpu_time(); 
			printf("(%llu) ", time);
			printf("unreghook:No reghook!\n");
		}
#endif
	}
}

static void
reghook (struct RTL8169_SUB_CTX *sctx, int i, u32 a, u32 b)
{
	u32 num;

#ifdef _DEBUG
	time = get_cpu_time(); 
	printf("(%llu) ", time);
	printf("reghook:start!\n");
#endif

	if(sctx == NULL)
	{
#ifdef _DEBUG
		time = get_cpu_time(); 
		printf("(%llu) ", time);
		printf("reghook:Error Parameter Invalid!\n");
#endif
	}
	else
	{	
		unreghook (sctx);
		sctx->i = i;
		sctx->e = 0;
		if (a == 0)		/* FIXME: is ignoring zero correct? */
			return;
		if ((a & PCI_CONFIG_BASE_ADDRESS_SPACEMASK) == PCI_CONFIG_BASE_ADDRESS_IOSPACE) {
			a &= PCI_CONFIG_BASE_ADDRESS_IOMASK;
			b &= PCI_CONFIG_BASE_ADDRESS_IOMASK;
			num = getnum (b);
			sctx->io = 1;
			sctx->ioaddr = a;
			sctx->hd = core_io_register_handler (a, num, rtl8169_io_handler, sctx, CORE_IO_PRIO_EXCLUSIVE, driver_name);
		}
		else 
		{
			a &= PCI_CONFIG_BASE_ADDRESS_MEMMASK;
			b &= PCI_CONFIG_BASE_ADDRESS_MEMMASK;
			num = getnum (b);
			sctx->mapaddr = a;
			sctx->maplen = num;
			sctx->map = mapmem_gphys (a, num, MAPMEM_WRITE | MAPMEM_PCD | MAPMEM_PWT);
			if (!sctx->map)
				panic ("mapmem failed");
			sctx->io = 0;
			sctx->h = mmio_register (a, num, rtl8169_mm_handler, sctx);
			if (!sctx->h)
				panic ("mmio_register failed");
			if (i == 1 || i == 2) {
				sctx->ctx->sctx_mmio = sctx;
				printf ("sctx[%d]=%p\n", i, sctx);
			}
		}
		sctx->e = 1;
	}
#ifdef _DEBUG
	time = get_cpu_time(); 
	printf("(%llu) ", time);
	printf("reghook:end!\n");
#endif
}

static int
rtl8169_offset_check(struct pci_device *dev, core_io_t io, u8 offset, union mem *data)
{
	int 		  ret = CORE_IO_RET_DONE;
	int 		  i;
	u32		  tmp;
	RTL8169_SUB_CTX *sctx;

#ifdef _DEBUG
	time = get_cpu_time(); 
	printf("(%llu) ", time);
	printf("rtl8169_offset_check:start!\n");
#endif

	if(dev == NULL || data == NULL)
	{
		ret = CORE_IO_RET_INVALID;
#ifdef _DEBUG
		time = get_cpu_time(); 
		printf("(%llu) ", time);
		printf("rtl8169_offset_check:Error Parameter Invalid!\n");
#endif
	}
	else
	{
		sctx = (RTL8169_SUB_CTX *)dev->host;

		if (offset + io.size - 1 >= 0x10 && offset <= 0x24) {
			if ((offset & 3) || io.size != 4)
				panic ("%s: io:%08x, offset=%02x, data:%08x\n",
				       __func__, *(int*)&io, offset, data->dword);
			i = (offset - 0x10) >> 2;
			ASSERT (i >= 0 && i < 6);
			tmp = dev->base_address_mask[i];
			if ((tmp & PCI_CONFIG_BASE_ADDRESS_SPACEMASK) == PCI_CONFIG_BASE_ADDRESS_IOSPACE)
				tmp &= data->dword | 3;
			else
				tmp &= data->dword | 0xF;
			reghook (&sctx[i], i, tmp, dev->base_address_mask[i]);
		}
		ret = CORE_IO_RET_DEFAULT;
#ifdef _DEBUG
		time = get_cpu_time(); 
		printf("(%llu) ", time);
		printf("rtl8169_offset_check:end!\n");
#endif
	}
	return ret;
}
//
// レジスタから指定されたサイズの値を読み込む
//
static unsigned int
rtl8169_read(RTL8169_SUB_CTX *sctx, phys_t offset, UINT size)
{
	UINT data = 0;
	
#ifdef _DEBUG
	time = get_cpu_time(); 
	printf("(%llu) ", time);
	printf("rtl8169_read:start!\n");
#endif
	// 引数チェック
	if (sctx == NULL)
	{
#ifdef _DEBUG
		time = get_cpu_time(); 
		printf("(%llu) ", time);
		printf("rtl8169_read:Error Parameter Invalid!\n");
#endif
		return 0;
	}

	if (offset >= sctx->maplen)
		panic ("rtl8169_read: offset %u >= sctx->maplen %u",
		       (unsigned int)offset, (unsigned int)sctx->maplen);
	memcpy (&data, (u8 *)sctx->map + offset, size);

#ifdef _DEBUG
	time = get_cpu_time(); 
	printf("(%llu) ", time);
	printf("rtl8169_read:end!\n");
#endif

	return data;
}

//
// レジスタに指定されたサイズの値を書き込む
//
static void
rtl8169_write(RTL8169_SUB_CTX *sctx, phys_t offset, UINT data, UINT size)
{
#ifdef _DEBUG
	time = get_cpu_time(); 
	printf("(%llu) ", time);
	printf("rtl8169_write:start!\n");
#endif

	// 引数チェック
	if (sctx == NULL)
	{
#ifdef _DEBUG
		time = get_cpu_time(); 
		printf("(%llu) ", time);
		printf("rtl8169_write:Error Parameter Invalid!\n");
#endif
		return;
	}
	if (offset >= sctx->maplen)
		panic ("rtl8169_write: offset %u >= sctx->maplen %u",
		       (unsigned int)offset, (unsigned int)sctx->maplen);
	if (sctx->map == NULL) {
		panic ("sctx->map == NULL!! sctx=%p sctx->i=%d\n", sctx,
		       sctx->i);
	}
	memcpy ((u8 *)sctx->map + offset, &data, size);

#ifdef _DEBUG
	time = get_cpu_time(); 
	printf("(%llu) ", time);
	printf("rtl8169_write:end!\n");
#endif

}
//
//レジスタ読み込みフックルーチン
//
// 戻り値
// false : レジスタへの読み込みを通常時と同様に行う
// true  : レジスタへの読み込みを行わない
//
static bool
rtl8169_hook_read(RTL8169_SUB_CTX *sctx, phys_t offset, UINT *data, UINT len)
{
	u16		read_data;
	RTL8169_CTX	*ctx;

#ifdef _DEBUG
	time = get_cpu_time(); 
	printf("(%llu) ", time);
	printf("rtl8169_hook_read:start!\n");
#endif

	//引数チェック
	if(sctx == NULL || data == NULL)
	{
#ifdef _DEBUG
		time = get_cpu_time(); 
		printf("(%llu) ", time);
		printf("rtl8169_hook_read:Error Parameter Invalid!\n");
#endif
		return false;
	}
	else if(sctx->ctx == NULL)
	{
#ifdef _DEBUG
		time = get_cpu_time(); 
		printf("(%llu) ", time);
		printf("rtl8169_hook_read:Error Parameter Invalid!\n");
#endif
		return false;
	}

	ctx = sctx->ctx;

	//レジスタの読み込み先チェック
	if (offset == RTL8169_REG_TNPDS) {
		memcpy (&data, &ctx->TNPDSreg, len);
		return true;
	}
	else if (offset == RTL8169_REG_THPDS) {
		memcpy (&data, &ctx->THPDSreg, len);
		return true;
	}
	else if (offset == RTL8169_REG_RDSAR) {
		memcpy (&data, &ctx->RDSARreg, len);
		return true;
	}
	if (offset != RTL8169_REG_ISR)
	{
#ifdef _DEBUG
		time = get_cpu_time(); 
		printf("(%llu) ", time);
		printf("rtl8169_hook_read:stop!\n");
#endif
		//割り込みステータスレジスタへの読み込みではないため、通常に処理する
		return false;
	}

	//VPNクライアント初期化前にフック処理を行おうとしないため
	if(!ctx->vpn_inited)
	{
#ifdef _DEBUG
		time = get_cpu_time(); 
		printf("(%llu) ", time);
		printf("rtl8169_hook_read:stop!\n");
#endif
		return false;
	}

	// 割り込みステータスレジスタの受信OKビットをチェック
	read_data = rtl8169_read(sctx, RTL8169_REG_ISR, 2);
	*data     =   read_data;
	if(read_data & RTL8169_REG_ISR_SWINT)
	{
		//受信割り込みが来た
		//ゲストOSにROKを通知
		*data |= RTL8169_REG_ISR_ROK;
#ifdef _DEBUG
		time = get_cpu_time(); 
		printf("(%llu) ", time);
		printf("rtl8169_hook_read:stop!\n");
#endif
		return true;
	}	
	else if (!(read_data & RTL8169_REG_ISR_ROK))
	{
#ifdef _DEBUG
		time = get_cpu_time(); 
		printf("(%llu) ", time);
		printf("rtl8169_hook_read:stop!\n");
#endif
		return true;
	}

	//受信ディスクリプタのデータをVPNクライアントに渡す
	rtl8169_get_rxdata_to_vpn(ctx, sctx);
#ifdef _DEBUG
	time = get_cpu_time(); 
	printf("(%llu) ", time);
	printf("rtl8169_hook_read:end!\n");
#endif
	
	return true;
}
static void
rtl8169_get_rxdata_to_vpn(RTL8169_CTX *ctx, RTL8169_SUB_CTX *sctx)
{
	bool	RxDescRing;
	int	i;
	int	ArrayNum = 0;
	UINT	optsl;
	struct desc *rxdesc;
	struct desc *TargetDesc = NULL;
	UINT	BufSize;

#ifdef _DEBUG
	time = get_cpu_time(); 
	printf("(%llu) ", time);
	printf("rtl8169_get_rxdata_to_vpn:start!\n");
#endif

	//引数チェック
	if(ctx == NULL || sctx == NULL)
	{
#ifdef _DEBUG
		time = get_cpu_time(); 
		printf("(%llu) ", time);
		printf("rtl8169_get_rxdata_to_vpn:Error Parameter Invalid!\n");
#endif
		return;
	}
	else if(ctx->CallbackRecvPhyNic == NULL)
	{
#ifdef _DEBUG
		time = get_cpu_time(); 
		printf("(%llu) ", time);
		printf("rtl8169_get_rxdata_to_vpn:Error Parameter Invalid!\n");
#endif
		return;
	}
	if (!recvenabled (ctx))
		return;

	rxdesc = ctx->RDSARvirt;
	if(!rxdesc)
	{
#ifdef _DEBUG
		time = get_cpu_time(); 
		printf("(%llu) ", time);
		printf("rtl8169_get_rxdata_to_vpn:Error Get RxData Vir Addr!\n");
#endif
		return;
	}


	for(i = 0; i < 256; i++)
	{
		optsl      = rxdesc[i].opts;
		TargetDesc = &rxdesc[i];
		BufSize    = TargetDesc->opts & 0x00003FFF;
		if(!(optsl & OPT_OWN))
		{
			if((optsl & OPT_FS) && (optsl & OPT_LS))
			{
				rtl8169_get_rxdesc_data(ctx, &ArrayNum, TargetDesc, BufSize, ctx->rdbufvirt[i]);
			}
			else if(optsl & OPT_FS)
			{
				rtl8169_get_rxdesc_data(ctx, &ArrayNum, TargetDesc, BufSize, ctx->rdbufvirt[i]);
				RxDescRing = true;
			}
			else if(optsl & OPT_LS)
			{
				rtl8169_get_rxdesc_data(ctx, &ArrayNum, TargetDesc, BufSize, ctx->rdbufvirt[i]);
				RxDescRing = false;			
			}
			else if(RxDescRing)
			{
				rtl8169_get_rxdesc_data(ctx, &ArrayNum, TargetDesc, BufSize, ctx->rdbufvirt[i]);
			}
		}

		if(optsl & OPT_EOR)
		{
			break;
		}
	}

#ifdef _DEBUG
	time = get_cpu_time(); 
	printf("(%llu) ", time);
	printf("rtl8169_get_rxdata_to_vpn:end!\n");
#endif

	return;
}

static void
rtl8169_get_rxdesc_data(RTL8169_CTX *ctx, int *ArrayNum, struct desc *TargetDesc, UINT BufSize, void *RxBuf)
{
	/*void		*RxBuf = NULL;*/
	bool		bret   = false;
	unsigned char	*chBuf;
	unsigned int	TotalSize;

#ifdef _DEBUG
	time = get_cpu_time(); 
	printf("(%llu) ", time);
	printf("rtl8169_get_rxdesc_data:start!\n");
#endif

	if(ctx == NULL || ArrayNum == NULL || TargetDesc == NULL)
	{
#ifdef _DEBUG
		time = get_cpu_time(); 
		printf("(%llu) ", time);
		printf("rtl8169_get_rxdesc_data:Error Parameter Invalid!\n");
#endif
		return;	
	}
	//受信用バッファをマップする
	//RxBuf = mapmem_gphys (TargetDesc->addrlow, BufSize, MAPMEM_WRITE | 0/*MAPMEM_PCD | MAPMEM_PWT*/ | 0/*MAPMEM_PAT*/);
	if(RxBuf)
	{
		chBuf = (unsigned char *)RxBuf;
		//ヘッダ判定(IP若しくはARPかチェックする)
		/*if(chBuf[ETHER_TYPE_FIRST] == 0x8 && (chBuf[ETHER_TYPE_LAST] == 0x0 || chBuf[ETHER_TYPE_LAST] == 0x6))*/if(1)
		{
			TotalSize = BufSize - 4;
			
			if(TotalSize <= BufSize)
			{
				ctx->RxBufAddr[0]   = RxBuf;
				ctx->RxBufSize[0]   = TotalSize;
				ctx->RxTmpSize[0]   = BufSize;
				/* TargetDesc->opts1 |= OPT_OWN; */
				ctx->rxtmpdesc[0]   = TargetDesc;
				bret = true;
				ctx->CallbackRecvPhyNic(ctx, 1, ctx->RxBufAddr, ctx->RxBufSize, ctx->CallbackRecvPhyNicParam);
				TargetDesc->opts &= OPT_EOR;
				TargetDesc->opts |= OPT_OWN | 0xFFF;
			}
		}
#ifdef _DEBUG
		else
		{
			time = get_cpu_time(); 
			printf("(%llu) ", time);
			printf("rtl8169_get_rxdesc_data:RxData Type Failed!\n");
		}
#endif
		//条件に満たないデータを解放する
		if(bret == false)
		{
			/*unmapmem(RxBuf, BufSize);*/
		}
#ifdef _DEBUG
		else
		{
			time = get_cpu_time(); 
			printf("(%llu) ", time);
			printf("rtl8169_get_rxdesc_data:end!\n");
		}
#endif
	}
#ifdef _DEBUG
	else
	{
		time = get_cpu_time(); 
		printf("(%llu) ", time);
		printf("rtl8169_get_rxdesc_data:Error Module mapmem_gphys!\n");
	}
#endif

	return;
}

//
//レジスタ書き込みフックルーチン
//
// 戻り値
// false : レジスタへの書き込みを通常時と同様に行う
// true  : レジスタへの書き込みを行わない
//
static bool
rtl8169_hook_write(RTL8169_SUB_CTX *sctx, phys_t offset, UINT data, UINT len)
{
	bool		bret;
	bool		bResult;
	RTL8169_CTX	*ctx;

#ifdef _DEBUG
	time = get_cpu_time(); 
	printf("(%llu) ", time);
	printf("rtl8169_hook_write:start!\n");
#endif

	//引数チェック
	if(sctx == NULL){
#ifdef _DEBUG
		time = get_cpu_time(); 
		printf("(%llu) ", time);
		printf("rtl8169_hook_write:Error Parameter Invalid!\n");
#endif
		return false;
	}
	else if(sctx->ctx == NULL){
#ifdef _DEBUG
		time = get_cpu_time(); 
		printf("(%llu) ", time);
		printf("rtl8169_hook_write:Error Parameter Invalid!\n");
#endif
		return false;
	}
	
	ctx = sctx->ctx;

	if ((offset == RTL8169_REG_TPPOLL) && (len == 1))
	{
		if (data & RTL8169_REG_TPPOLL_HPQ)
		{
			bret = rtl8169_get_txdata_to_vpn(ctx, sctx, RTL8169_TX_HIGH_PRIORITY_DESC);
#ifdef _DEBUG
			time = get_cpu_time(); 
			printf("(%llu) ", time);
			if(bret == true)//成功
			{			
				printf("rtl8169_hook_write:end!\n");
			}
			else//失敗
			{
				printf("rtl8169_hook_write:Error Get TxDesc Data!\n");
			}
#endif
		}
		else if (data & RTL8169_REG_TPPOLL_NPQ)
		{
			bret = rtl8169_get_txdata_to_vpn(ctx, sctx, RTL8169_TX_NORMAL_PRIORITY_DESC);
#ifdef _DEBUG
			time = get_cpu_time(); 
			printf("(%llu) ", time);
			if(bret == true)//成功
			{			
				printf("rtl8169_hook_write:end!\n");
			}
			else//失敗
			{
				printf("rtl8169_hook_write:Error Get TxDesc Data!\n");
			}
#endif
		}
		else
		{
#ifdef _DEBUG
			time = get_cpu_time(); 
			printf("(%llu) ", time);
			printf("rtl8169_hook_write:Error Status Invalid!\n");
#endif
			bret = false;
		}
	}
	else if (offset == RTL8169_REG_CR)
	{
		if ((unsigned char)data == 0x0C)
		{
			// CRレジスタにReset/RxEnable/TxEnableが書き込まれた
			sctx->hwReset = true;
		}
		if ((unsigned char)data == 0x00) {
			ctx->enableflag = 0;
			printf ("Down\n");
		}
		bret = false;
#ifdef _DEBUG
		time = get_cpu_time(); 
		printf("(%llu) ", time);
		printf("rtl8169_hook_write:end!\n");
#endif
	}
	else if (offset == RTL8169_REG_RCR)
	{
		bret = false;
		if((!ctx->vpn_inited) && (data == RTL8169_VPN_INIT_ACCESS_L || data == RTL8169_VPN_INIT_ACCESS_W) && (sctx->hwReset))		
		{
			rtl8169_write(sctx, RTL8169_REG_RCR, data, len);
			bret    = true;
			bResult = rtl8169_init_vpn_client(ctx, sctx);
#ifdef _DEBUG
			time = get_cpu_time(); 
			printf("(%llu) ", time);
			if(bResult == true)//VPN初期化成功
			{
				printf("rtl8169_hook_write:end!\n");
			}
			else//VPN初期化失敗
			{
				printf("rtl8169_hook_write:Error VPN Client Initialization!\n");
			}
#endif			
		}
	}
	else if (offset == RTL8169_REG_TNPDS) {
		memcpy (&ctx->TNPDSreg, &data, len);
		rtl8169_write(sctx, RTL8169_REG_TNPDS, ctx->TNPDSphys, 4);
		ctx->sendindex = 0;
		ctx->enableflag |= 1;
		printf ("TNPDS\n");
		bret = true;
	}
	else if (offset == RTL8169_REG_THPDS) {
		memcpy (&ctx->THPDSreg, &data, len);
		rtl8169_write(sctx, RTL8169_REG_THPDS, ctx->THPDSphys, 4);
		ctx->enableflag |= 1;
		printf ("THPDS\n");
		bret = true;
	}
	else if (offset == RTL8169_REG_RDSAR) {
		memcpy (&ctx->RDSARreg, &data, len);
		rtl8169_write(sctx, RTL8169_REG_RDSAR, ctx->RDSARphys, 4);
		ctx->RxDescNum = 0;
		ctx->enableflag |= 2;
		printf ("RDSAR\n");
		bret = true;
	}
	else //それ以外のレジスタアクセス
	{
		bret = false;
#ifdef _DEBUG
		time = get_cpu_time(); 
		printf("(%llu) ", time);
		printf("rtl8169_hook_write:end!\n");
#endif
	}

	return bret;
}

static void
udpcs (u8 *buf, unsigned int len)
{
	unsigned int iplen, udplen, datalen;
	u16 *cs;

	if ((buf[14] & 0xF0) != 0x40)
		return;
	iplen = (buf[14] & 0x0F) * 4;
	udplen = 8;
	datalen = (buf[17] | buf[16] << 8) - iplen - udplen;
	cs = (u16 *)&buf[14 + iplen + 6];
	*cs = ipchecksum (&buf[14 + iplen], udplen + datalen);
}

static void
tcpcs (u8 *buf, unsigned int len)
{
	unsigned int iplen, tcplen, datalen;
	u16 *cs;

	if ((buf[14] & 0xF0) != 0x40)
		return;
	iplen = (buf[14] & 0x0F) * 4;
	tcplen = (buf[14 + iplen + 12] >> 8) * 4;
	datalen = (buf[17] | buf[16] << 8) - iplen - tcplen;
	cs = (u16 *)&buf[14 + iplen + 16];
	*cs = ipchecksum (&buf[14 + iplen], tcplen + datalen);
}

static void
ipcs (u8 *buf, unsigned int len)
{
	unsigned int iplen;
	u16 *cs;

	if ((buf[14] & 0xF0) != 0x40)
		return;
	iplen = (buf[14] & 0x0F) * 4;
	cs = (u16 *)&buf[24];
	*cs = 0;
	*cs = ipchecksum (&buf[14], iplen);
}

static bool
rtl8169_get_txdata_to_vpn(RTL8169_CTX *ctx, RTL8169_SUB_CTX *sctx, int Desckind)
{
	int	i;
	UINT	optsl;
	UINT	BufSize;
	phys_t txdescphys;
	struct desc *TargetDesc = NULL;
	void	*TxBuf = NULL;
	int num_received = 0;
	

#ifdef _DEBUG
	time = get_cpu_time(); 
	printf("(%llu) ", time);
	printf("rtl8169_get_txdate_to_vpn:start!\n");
#endif

	if(ctx == NULL || sctx == NULL)
	{
#ifdef _DEBUG
		time = get_cpu_time(); 
		printf("(%llu) ", time);
		printf("rtl8169_get_txdate_to_vpn:Error Parameter Invalid!\n");
#endif
		return false;
	}
	else if(ctx->CallbackRecvVirtNic == NULL)
	{
#ifdef _DEBUG
		time = get_cpu_time(); 
		printf("(%llu) ", time);
		printf("rtl8169_get_txdate_to_vpn:Error Parameter Invalid!\n");
#endif
		return false;
	}
	if (!sendenabled (ctx))
		return false;

	if (Desckind == RTL8169_TX_HIGH_PRIORITY_DESC)
		txdescphys = sctx->ctx->THPDSreg;
	else
		txdescphys = sctx->ctx->TNPDSreg;

	for(i = 0; i < RTL8169_TXDESC_MAX_NUM; i++)
	{
		TargetDesc = mapmem_gphys (txdescphys + sizeof *TargetDesc * i,
					   sizeof *TargetDesc, MAPMEM_WRITE);
		if (!TargetDesc) {
			printf ("mapmem err txdescphys=0x%llX i=%d\n",
				txdescphys, i);
			break;
		}
		optsl      = TargetDesc->opts;
		BufSize    = TargetDesc->opts & 0x00003FFF;
		if ((optsl & OPT_OWN) && BufSize)
		{
			//送信用バッファをマップする
			TxBuf = mapmem_gphys (TargetDesc->addr, BufSize, MAPMEM_WRITE | 0/*MAPMEM_PCD | MAPMEM_PWT*/ | 0/*MAPMEM_PAT*/);
			if (optsl & OPT_LGSND)
				panic ("LGSND=1 opts1=0x%08X Target=%p  txdescphys=0x%llX i=%d, kind=%d", optsl, TargetDesc, txdescphys, i, (int)Desckind);
			if (!(optsl & (OPT_LGSND | OPT_IPCS | OPT_UDPCS |
				       OPT_TCPCS)) && (optsl & OPT_FS) &&
			    (optsl & OPT_LS)) {
				/* shortcut */
				if (ctx->TxBufAddr[0])
					free (ctx->TxBufAddr[0]);
				ctx->TxBufAddr[0] = TxBuf;
				ctx->TxBufSize[0] = BufSize;
				ctx->CallbackRecvVirtNic(ctx, 1, ctx->TxBufAddr, ctx->TxBufSize, ctx->CallbackRecvVirtNicParam);
				ctx->TxBufAddr[0] = NULL;
			} else {
				if (optsl & OPT_FS) {
					if (ctx->TxBufAddr[0])
						free (ctx->TxBufAddr[0]);
					ctx->TxBufAddr[0] = alloc (BufSize);
					ctx->TxBufSize[0] = BufSize;
					memcpy (ctx->TxBufAddr[0], TxBuf,
						BufSize);
				} else if (ctx->TxBufAddr[0]) {
					ctx->TxBufAddr[0] = realloc (ctx->
								     TxBufAddr
								     [0],
								     ctx->
								     TxBufSize
								     [0] +
								     BufSize);
					memcpy ((u8 *)ctx->TxBufAddr[0] +
						ctx->TxBufSize[0], TxBuf,
						BufSize);
					ctx->TxBufSize[0] += BufSize;
				}
				if (ctx->TxBufAddr[0] && (optsl & OPT_LS)) {
					if (optsl & OPT_UDPCS)
						udpcs (ctx->TxBufAddr[0],
						       ctx->TxBufSize[0]);
					if (optsl & OPT_TCPCS)
						tcpcs (ctx->TxBufAddr[0],
						       ctx->TxBufSize[0]);
					if (optsl & OPT_IPCS)
						ipcs (ctx->TxBufAddr[0],
						      ctx->TxBufSize[0]);
					ctx->CallbackRecvVirtNic(ctx, 1, ctx->TxBufAddr, ctx->TxBufSize, ctx->CallbackRecvVirtNicParam);
					free (ctx->TxBufAddr[0]);
					ctx->TxBufAddr[0] = NULL;
				}
			}
			unmapmem (TxBuf, BufSize);
			TargetDesc->opts &= ~OPT_OWN;
			num_received++;
		}
		unmapmem (TargetDesc, sizeof *TargetDesc);

		if(optsl & OPT_EOR)
		{
			break;
		}
	}

#ifdef _DEBUG
	time = get_cpu_time(); 
	printf("(%llu) ", time);
	printf("rtl8169_get_txdate_to_vpn:end!\n");
#endif

	if (num_received >= 1)
		makeintr (sctx);

	return true;
}

//
// VPNクライアントの初期化
//
static bool
rtl8169_init_vpn_client(RTL8169_CTX *ctx, RTL8169_SUB_CTX *sctx)
{
	bool bret = false;

#ifdef _DEBUG
	time = get_cpu_time(); 
	printf("(%llu) ", time);
	printf("rtl8169_init_vpn_client:start!\n");
#endif

	if(ctx == NULL || sctx == NULL)
	{
#ifdef _DEBUG
		time = get_cpu_time(); 
		printf("(%llu) ", time);
		printf("rtl8169_init_vpn_client:Error Parameter Invalid!\n");
#endif
	}
	else
	{
		if(ctx->vpn_inited)
		{
#ifdef _DEBUG
			time = get_cpu_time(); 
			printf("(%llu) ", time);
			printf("rtl8169_init_vpn_client:Already Init Vpn!\n");
#endif
			bret = true;
		}
		else
		{
			bret = rtl8169_get_macaddr(sctx,ctx->macaddr);
			if(!bret)
			{
#ifdef _DEBUG
				time = get_cpu_time(); 
				printf("(%llu) ", time);
				printf("rtl8169_init_vpn_client:Error Module rtl8169_get_macaddr!\n");
#endif
			}
			else
			{
			 	ctx->vpn_handle = vpn_new_nic((SE_HANDLE)ctx, (SE_HANDLE)ctx, &func);
				if(ctx->vpn_handle == NULL)
				{	
					bret = false;
#ifdef _DEBUG
					time = get_cpu_time(); 
					printf("(%llu) ", time);
					printf("rtl8169_init_vpn_client:Error Module vpn_new_nic!\n");
#endif
				}
				else
				{
#ifdef _DEBUG
					time = get_cpu_time(); 
					printf("(%llu) ", time);
					printf("rtl8169_init_vpn_client:end!\n");
#endif
					ctx->vpn_inited = true;
				}
			}
		}
	}

	return bret;
}

//
// メモリマップドレジスタ用フックハンドラ
//
static int
rtl8169_mm_handler(void *data, phys_t gphys, bool wr, void *buf, uint len, u32 flags)
{
	RTL8169_SUB_CTX *sctx;
	RTL8169_CTX     *ctx;
	int		  ret = 0;
	phys_t		  offset;
	UINT 		  ret_data = 0;
	UINT		  in_data  = 0;

#ifdef _DEBUG
	time = get_cpu_time(); 
	printf("(%llu) ", time);
	printf("rtl8169_mm_handler:start!\n");
#endif

	if(data == NULL || buf == NULL)
	{
#ifdef _DEBUG
		time = get_cpu_time(); 
		printf("(%llu) ", time);
		printf("rtl8169_mm_handler:Error Parameter Invalid!\n");
#endif
	}
	else
	{
		sctx = (RTL8169_SUB_CTX *)data;
		ctx  = sctx->ctx;
		spinlock_lock (&ctx->lock);

		if((u32)sctx->mapaddr <= (u32)gphys && (u32)gphys < (u32)sctx->mapaddr + RTL8169_REGISTER_SIZE)
		{
			offset  = gphys - sctx->mapaddr;
			if (len == 1 || len == 2 || len == 4)
			{
				if (wr == 0)
				{
					ret_data = 0;
					if (rtl8169_hook_read(sctx, offset, &ret_data, len) == false)
					{
						ret_data = rtl8169_read(sctx, offset, len);
					}

					if (len == 1)
					{
						*((UCHAR *)buf) = (UCHAR)ret_data;
					}
					else if (len == 2)
					{
						*((USHORT *)buf) = (USHORT)ret_data;
					}
					else if (len == 4)
					{
						*((UINT *)buf) = (UINT)ret_data;
					}
				}
				else
				{
					data = 0;
					if (len == 1)
					{
						in_data = (UINT)(*((UCHAR *)buf));
					}
					else if (len == 2)
					{
						in_data = (UINT)(*((USHORT *)buf));
					}
					else if (len == 4)
					{
						in_data = (UINT)(*((UINT *)buf));
					}

					if (rtl8169_hook_write(sctx, offset, in_data, len) == false)
					{
						rtl8169_write(sctx, offset, in_data, len);
					}
				}
#ifdef _DEBUG
				time = get_cpu_time(); 
				printf("(%llu) ", time);
				printf("rtl8169_mm_handler:end!\n");
#endif
			}
#ifdef _DEBUG
			else
			{
				time = get_cpu_time(); 
				printf("(%llu) ", time);
				printf("rtl8169_mm_handler:Data Size Error!\n");
			}
#endif
			ret = 1;
		}
#ifdef _DEBUG
		else
		{
			time = get_cpu_time(); 
			printf("(%llu) ", time);
			printf("rtl8169_mm_handler:No Memory Map Failed!\n");
		}
#endif
		spinlock_unlock (&ctx->lock);
	}

	return ret;
}
//
// I/Oマップドレジスタ用フックハンドラ
//
static int
rtl8169_io_handler(core_io_t io, union mem *data, void *arg)
{
#ifdef _DEBUG
	time = get_cpu_time(); 
	printf("(%llu) ", time);
	printf("rtl8169_io_handler:end!\n");
#endif
	return CORE_IO_RET_DEFAULT;
}
//
// PCI コンフィグレーションレジスタの読み込み処理
//
static int
rtl8169_config_read_sub (struct pci_device *dev, core_io_t io,
			 u8 offset, union mem *data)
{
	int ret = CORE_IO_RET_DONE;

#ifdef _DEBUG
	time = get_cpu_time(); 
	printf("(%llu) ", time);
	printf("rtl8169_config_read:start!\n");
#endif

	if(dev == NULL || data == NULL)
	{
		ret = CORE_IO_RET_INVALID;
#ifdef _DEBUG
		time = get_cpu_time(); 
		printf("(%llu) ", time);
		printf("rtl8169_config_read:Error Parameter Invalid!\n");
#endif
	}
	else
	{	
		core_io_handle_default(io, data);
#ifdef _DEBUG
		time = get_cpu_time(); 
		printf("(%llu) ", time);
		printf("rtl8169_config_read:end!\n");
#endif
	}
	return ret;
}

//
// PCI コンフィグレーションレジスタの書き込み処理
//
static int
rtl8169_config_write_sub (struct pci_device *dev, core_io_t io,
			  u8 offset, union mem *data)
{
	int ret = CORE_IO_RET_DONE;

#ifdef _DEBUG
	time = get_cpu_time(); 
	printf("(%llu) ", time);
	printf("rtl8169_config_write:start!\n");
#endif

	if(dev == NULL || data == NULL)
	{
		ret = CORE_IO_RET_INVALID;
#ifdef _DEBUG
		time = get_cpu_time(); 
		printf("(%llu) ", time);
		printf("rtl8169_config_write:Error Parameter Invalid!\n");
#endif
	}
	else
	{
		ret = rtl8169_offset_check(dev, io, offset, data);
#ifdef _DEBUG
		time = get_cpu_time(); 
		printf("(%llu) ", time);
		printf("rtl8169_config_write:end!\n");
#endif
	}
	return ret;	
}

//
// 新しいデバイスの検出
//
static void
rtl8169_new_sub (struct pci_device *dev)
{
	RTL8169_CTX		*ctx;
	RTL8169_SUB_CTX	*sctx;
	int			i;
	struct desc *rdsar;
	struct desc *tdesc;

#ifdef _DEBUG
	time = get_cpu_time(); 
	printf("(%llu) ", time);
	printf("rtl8169_new:start!\n");
#endif

	if(dev == NULL)
	{
#ifdef _DEBUG
		time = get_cpu_time(); 
		printf("(%llu) ", time);
		printf("rtl8169_new:Error Parameter Invalid!\n");
#endif
	}
	else
	{
		ctx  = alloc (sizeof *ctx);
		memset (ctx, 0, sizeof *ctx);
		//TNPDSのシャドウ物理アドレスの仮想メモリ領域(4096KB = 送信ディスクリプタ256個分)を取得
		alloc_pages (&ctx->TNPDSvirt, &ctx->TNPDSphys, 1);
		alloc_pages (&ctx->THPDSvirt, &ctx->THPDSphys, 1);
		alloc_pages (&ctx->RDSARvirt, &ctx->RDSARphys, 1);
		tdesc = ctx->TNPDSvirt;
		for (i = 0; i < 256; i++) {
			alloc_page (&ctx->tnbufvirt[i], &ctx->tnbufphys[i]);
			tdesc[i].addr = ctx->tnbufphys[i];
			tdesc[i].opts = 0xFFF;
		}
		tdesc[i - 1].opts |= OPT_EOR;
		tdesc = ctx->THPDSvirt;
		for (i = 0; i < 256; i++) {
			alloc_page (&ctx->thbufvirt[i], &ctx->thbufphys[i]);
			tdesc[i].addr = ctx->thbufphys[i];
			tdesc[i].opts = 0xFFF;
		}
		tdesc[i - 1].opts |= OPT_EOR;
		rdsar = ctx->RDSARvirt;
		for (i = 0; i < 256; i++) {
			alloc_page (&ctx->rdbufvirt[i], &ctx->rdbufphys[i]);
			rdsar[i].addr = ctx->rdbufphys[i];
			rdsar[i].opts = OPT_OWN | 0xFFF;
		}
		rdsar[i - 1].opts |= OPT_EOR;
		ctx->TxBufAddr[0] = NULL;

		sctx = alloc (sizeof *sctx * 6);
		memset (sctx, 0, sizeof *sctx * 6);
		printf ("ctx = %p\n", ctx);
		for(i=0; i < 6; i++)
		{
			printf ("&sctx[%d] = %p\n", i, &sctx[i]);
			sctx[i].ctx = ctx;
			sctx[i].e   = 0;
			reghook (&sctx[i], i, dev->config_space.base_address[i], dev->base_address_mask[i]);
		}
		ctx->sctx = sctx;
		dev->host = sctx;
		dev->driver->options.use_base_address_mask_emulation = 1;
#ifdef _DEBUG
		time = get_cpu_time(); 
		printf("(%llu) ", time);
		printf("rtl8169_new:end!\n");
#endif
		return;
	}
}

#ifdef TTY_RTL8169
#define printd(X...) do { if (0) printf (X); } while (0)

static void
rtl8169_tty_send (void *handle, void *packet, unsigned int packet_size)
{
	char *pkt;
	RTL8169_CTX *ctx;

	ctx = handle;
	if (!sendenabled (ctx))
		return;
	pkt = packet;
	memcpy (pkt + 0, config.vmm.tty_rtl8169_mac_address, 6);
	memcpy (pkt + 6, ctx->macaddr, 6);
	SendPhysicalNic ((SE_HANDLE)ctx, 1, &packet, &packet_size);
}

/* reset RTL8169 controller */
static void
rtl8169_reset (RTL8169_SUB_CTX *sctx, RTL8169_CTX *ctx)
{
	int i;

	printf ("Resetting RTL8169 controller...");
	rtl8169_write (sctx, RTL8169_REG_CR, 0x10, 1);
	do {
		printf (".");
	} while (rtl8169_read (sctx, RTL8169_REG_CR, 1) & 0x10);
	printf ("done.\n");

	printd ("Get MAC addr.\n");
	if (!rtl8169_get_macaddr (sctx, ctx->macaddr))
		panic ("failed to get mac addr");
	printf ("MacAddr: %02x", ctx->macaddr[0]);
	for (i = 1; i < 6; i++)
		printf (":%02x", ctx->macaddr[i]);
	printf ("\n");

	printd ("Disable interrupt.\n");
	rtl8169_write (sctx, RTL8169_REG_ISR,
		       0xffff, 2); /* clear pending interrupts */
	rtl8169_write (sctx, RTL8169_REG_IMR,
		       0, 2);	/* disable all interrupts */

	printd ("Enable transmit.\n");
	/* set descriptors */
	rtl8169_write (sctx, RTL8169_REG_TNPDS, ctx->TNPDSphys,
		       4); /* normal */
	rtl8169_write (sctx, RTL8169_REG_CR, 1 << 2, 1); /* transmit enabled */
	ctx->sendindex = 0;
	ctx->enableflag |= 1;

	printd ("Start tty via RTL8169.\n");
	tty_udp_register (rtl8169_tty_send, ctx);
}

static void
rtl8169_tty_init (struct pci_device *dev)
{
	RTL8169_SUB_CTX *sctx;

	/* get sub context */
	sctx = (RTL8169_SUB_CTX *) dev->host;
	sctx = sctx->ctx->sctx_mmio;	/* get sctx with mmio */
	if (!sctx)
		panic ("mmio not found");

	if (config.vmm.driver.vpn.RTL8169) /* vpn enabled */
		tty_udp_register (rtl8169_tty_send,
				  sctx->ctx); /* don't reset */
	else
		rtl8169_reset (sctx, sctx->ctx); /* do reset */
}
#endif	/* TTY_RTL8169 */

static int
rtl8169_config_read (struct pci_device *dev, core_io_t io,
		     u8 offset, union mem *data)
{
	if (config.vmm.driver.vpn.RTL8169) /* vpn enabled */
		return rtl8169_config_read_sub (dev, io, offset, data);
	data->dword = 0UL;
	return CORE_IO_RET_DONE;
}

static int
rtl8169_config_write (struct pci_device *dev, core_io_t io,
		      u8 offset, union mem *data)
{
	if (config.vmm.driver.vpn.RTL8169) /* vpn enabled */
		return rtl8169_config_write_sub (dev, io, offset, data);
	return CORE_IO_RET_DONE;
}

static void
rtl8169_new (struct pci_device *dev)
{
	rtl8169_new_sub (dev);
#ifdef TTY_RTL8169
	if (config.vmm.tty_rtl8169) /* tty enabled */
		rtl8169_tty_init (dev);
#endif	/* TTY_RTL8169 */
}

//
// RTL8169の初期化
//
static void
rtl8169_init()
{
	if (!config.vmm.driver.vpn.RTL8169 &&
	    !config.vmm.tty_rtl8169) /* disabled all of them */
		return;
#ifdef _DEBUG
	time = get_cpu_time(); 
	printf("(%llu) ", time);
	printf("rtl8169_init:start!\n");
#endif
	pci_register_driver(&vpn_rtl8169_driver);
#ifdef _DEBUG
	time = get_cpu_time(); 
	printf("(%llu) ", time);
	printf("rtl8169_init:end!\n");
#endif
}

PCI_DRIVER_INIT(rtl8169_init);

#endif /* VPN_RTL8169 */
#endif /* VPN */
