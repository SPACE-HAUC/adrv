
// https://ez.analog.com/linux-device-drivers/linux-software-drivers/f/q-a/
// 110319/qpsk-transmission-and-reception-using-example-c-code
// Accessed 5/13/19

/*
 * libiio - AD9361 IIO streaming example
 *
 * Copyright (C) 2014 IABG mbH
 * Author: Michael Feilen <feilen_at_iabg.de>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 *
 * Modified by DDR/RTY Oct 2016.
 *
 **/

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <math.h>
#include <iio.h>

/* helper macros */
#define MHZ(x) ((long long)(x*1000000.0 + .5))
#define GHZ(x) ((long long)(x*1000000000.0 + .5))

/* RX is input, TX is output */
enum iodev { RX, TX };

/* common RX and TX streaming params */
struct stream_cfg {
	long long bw_hz; // Analog banwidth in Hz
	long long fs_hz; // Baseband sample rate in Hz
	long long lo_hz; // Local oscillator frequency in Hz
	const char* rfport; // Port name
};

/* static scratch mem for strings */
static char tmpstr[64];

/* IIO structs required for streaming */
static struct iio_context *ctx   = NULL;
static struct iio_channel *rx0_i = NULL;
static struct iio_channel *rx0_q = NULL;
static struct iio_channel *tx0_i = NULL;
static struct iio_channel *tx0_q = NULL;
static struct iio_buffer  *rxbuf = NULL;
static struct iio_buffer  *txbuf = NULL;

static bool stop;

/* signal generator: make a sine wave */

/* cleanup and exit */
static void shutdown()
{
	printf("* Destroying buffers\n");
	if (rxbuf)
  {
    iio_buffer_destroy(rxbuf);
  }
	if (txbuf)
  {
    iio_buffer_destroy(txbuf);
  }

	printf("* Disabling streaming channels\n");
	if (rx0_i)
  {
    iio_channel_disable(rx0_i);
  }
	if (rx0_q)
  {
    iio_channel_disable(rx0_q);
  }
	if (tx0_i)
  {
    iio_channel_disable(tx0_i);
  }
	if (tx0_q)
  {
    iio_channel_disable(tx0_q);
  }

	printf("* Destroying context\n");
	if (ctx)
  {
    iio_context_destroy(ctx);
  }
	exit(0);
}

static void handle_sig(int sig)
{
	printf("Waiting for process to finish...\n");
	stop = true;
}

/* check return value of attr_write function */
static void errchk(int v, const char* what)
{
	 if (v < 0)
   {
     fprintf(stderr, "Error %d writing to channel \"%s\"\nvalue may not be supported.\n", v, what);
     shutdown();
   }
}

/* write attribute: long long int */
static void wr_ch_lli(struct iio_channel *chn, const char* what, long long val)
{
	errchk(iio_channel_attr_write_longlong(chn, what, val), what);
}

/* write attribute: string */
static void wr_ch_str(struct iio_channel *chn, const char* what, const char* str)
{
	errchk(iio_channel_attr_write(chn, what, str), what);
}

/* helper function generating channel names */
static char* get_ch_name(const char* type, int id)
{
	snprintf(tmpstr, sizeof(tmpstr), "%s%d", type, id);
	return tmpstr;
}

/* returns ad9361 phy device */
static struct iio_device* get_ad9361_phy(struct iio_context *ctx)
{
	struct iio_device *dev =  iio_context_find_device(ctx, "ad9361-phy");
	assert(dev && "No ad9361-phy found");
	return dev;
}

/* finds AD9361 streaming IIO devices */
static bool get_ad9361_stream_dev(struct iio_context *ctx, enum iodev d, struct iio_device **dev)
{
	switch (d)
  {
	   case TX:
     {
       *dev = iio_context_find_device(ctx, "cf-ad9361-dds-core-lpc");
       return *dev != NULL;
     }
	   case RX:
     {
       *dev = iio_context_find_device(ctx, "cf-ad9361-lpc");
       return *dev != NULL;
     }
	   default:
     {
       assert(0);
       return false;
     }
	}
}

/* finds AD9361 streaming IIO channels */
static bool get_ad9361_stream_ch(struct iio_context *ctx, enum iodev d, struct iio_device *dev, int chid, struct iio_channel **chn)
{
	*chn = iio_device_find_channel(dev, get_ch_name("voltage", chid), d == TX);
	if (!*chn)
  {
    *chn = iio_device_find_channel(dev, get_ch_name("altvoltage", chid), d == TX);
  }
	return *chn != NULL;
}

/* finds AD9361 phy IIO configuration channel with id chid */
static bool get_phy_chan(struct iio_context *ctx, enum iodev d, int chid, struct iio_channel **chn)
{
	switch (d)
  {
	   case RX:
     {
       *chn = iio_device_find_channel(get_ad9361_phy(ctx), get_ch_name("voltage", chid), false);
       return *chn != NULL;
     }
	   case TX:
     {
       *chn = iio_device_find_channel(get_ad9361_phy(ctx), get_ch_name("voltage", chid), true);
       return *chn != NULL;
     }
	   default:
     {
       assert(0);
       return false;
     }
	}
}

/* finds AD9361 local oscillator IIO configuration channels */
static bool get_lo_chan(struct iio_context *ctx, enum iodev d, struct iio_channel **chn)
{
	switch (d)
  {
	 // LO chan is always output, i.e. true
	case RX:
  {
    *chn = iio_device_find_channel(get_ad9361_phy(ctx), get_ch_name("altvoltage", 0), true);
    return *chn != NULL;
  }
	case TX:
  {
    *chn = iio_device_find_channel(get_ad9361_phy(ctx), get_ch_name("altvoltage", 1), true);
     return *chn != NULL;
  }
	default:
  {
    assert(0);
    return false;
	}
}

/* applies streaming configuration through IIO */
bool cfg_ad9361_streaming_ch(struct iio_context *ctx, struct stream_cfg *cfg, enum iodev type, int chid)
{
	struct iio_channel *chn = NULL;

	// Configure phy and lo channels
	printf("* Acquiring AD9361 phy channel %d\n", chid);
	if (!get_phy_chan(ctx, type, chid, &chn))
  {
    return false;
  }
	wr_ch_str(chn, "rf_port_select",     cfg->rfport);
	wr_ch_lli(chn, "rf_bandwidth",       cfg->bw_hz);
	wr_ch_lli(chn, "sampling_frequency", cfg->fs_hz);

	// Configure LO channel
	printf("* Acquiring AD9361 %s lo channel\n", type == TX ? "TX" : "RX");
	if (!get_lo_chan(ctx, type, &chn))
  {
    return false;
  }
	wr_ch_lli(chn, "frequency", cfg->lo_hz);
	return true;
}

/* simple configuration and streaming */
int main (int argc, char **argv)
{
	// Streaming devices
	struct iio_device *tx;
	struct iio_device *rx;

	// Stream configurations
	struct stream_cfg rxcfg;
	struct stream_cfg txcfg;

	// Listen to ctrl+c and assert
	signal(SIGINT, handle_sig);

	// RX stream config
	rxcfg.bw_hz = MHZ(16.0);   // 2 MHz rf bandwidth
	rxcfg.fs_hz = MHZ(30.72);   // 2.5 MS/s rx sample rate
	rxcfg.lo_hz = GHZ(2.4); // 2.5 GHz rf frequency
	rxcfg.rfport = "A_BALANCED"; // port A (select for rf freq.)

	// slow attack mode?

	// TX stream config
	txcfg.bw_hz = MHZ(16.0); // 1.5 MHz rf bandwidth
	txcfg.fs_hz = MHZ(30.72);   // 2.5 MS/s tx sample rate
	txcfg.lo_hz = GHZ(2.4); // 2.5 GHz rf frequency
	txcfg.rfport = "A"; // port A (select for rf freq.)


	printf("* Acquiring IIO context\n");
	assert((ctx = iio_create_default_context()) && "No context");
	assert(iio_context_get_devices_count(ctx) > 0 && "No devices");

	printf("* Acquiring AD9361 streaming devices\n");
	assert(get_ad9361_stream_dev(ctx, TX, &tx) && "No tx dev found");
	assert(get_ad9361_stream_dev(ctx, RX, &rx) && "No rx dev found");

	printf("* Configuring AD9361 for streaming\n");
	assert(cfg_ad9361_streaming_ch(ctx, &rxcfg, RX, 0) && "RX port 0 not found");
	assert(cfg_ad9361_streaming_ch(ctx, &txcfg, TX, 0) && "TX port 0 not found");

	printf("* Initializing AD9361 IIO streaming channels\n");
	assert(get_ad9361_stream_ch(ctx, RX, rx, 0, &rx0_i) && "RX chan i not found");
	assert(get_ad9361_stream_ch(ctx, RX, rx, 1, &rx0_q) && "RX chan q not found");
	assert(get_ad9361_stream_ch(ctx, TX, tx, 0, &tx0_i) && "TX chan i not found");
	assert(get_ad9361_stream_ch(ctx, TX, tx, 1, &tx0_q) && "TX chan q not found");

	printf("* Enabling IIO streaming channels\n");
	iio_channel_enable(rx0_i);
	iio_channel_enable(rx0_q);
	iio_channel_enable(tx0_i);
	iio_channel_enable(tx0_q);

	int buffer_size = 4200;

	printf("* Creating non-cyclic IIO buffers with 1 MiS\n");
	rxbuf = iio_device_create_buffer(rx, buffer_size, false);
	if (!rxbuf)
  {
		perror("Could not create RX buffer");
		shutdown();
	}
	txbuf = iio_device_create_buffer(tx, buffer_size, false);
	if (!txbuf)
  {
		perror("Could not create TX buffer");
		shutdown();
	}

	FILE *foutp = fopen("output.csv", "w+");
	FILE *finp = fopen("qpsk_1114newrc.txt", "r");
	int iter = 0;
	int16_t QPSKsamplesTX[4200][4];
	float i1,q1,i2,q2;
	for(iter = 0; iter < 4200; iter++)
  {
		fscanf(finp, "%f,%f,%f,%f\n", &i1,&q1,&i2,&q2);
		QPSKsamplesTX[iter][0]=(int16_t)i1*3200.0;
		QPSKsamplesTX[iter][1]=(int16_t)q1*3200.0;
		QPSKsamplesTX[iter][2]=(int16_t)i2*3200.0;
		QPSKsamplesTX[iter][3]=(int16_t)q2*3200.0;
	}

	printf("* Starting IO streaming (press CTRL+C to cancel)\n");
	while (!stop)
	{
		ssize_t nbytes_rx, nbytes_tx;
		void *p_dat, *p_end;
		ptrdiff_t p_inc;

		// Schedule TX buffer: send to hardware
		nbytes_tx = iio_buffer_push(txbuf);
		if (nbytes_tx < 0)
    {
      printf("Error pushing buf %d\n", (int) nbytes_tx);
      shutdown();
    }

		// Refill RX buffer: fetch from hardware
		nbytes_rx = iio_buffer_refill(rxbuf);
		if (nbytes_rx < 0)
    {
      printf("Error refilling buf %d\n",(int) nbytes_rx);
      shutdown();
    }

		p_inc = iio_buffer_step(rxbuf);
		p_end = iio_buffer_end(rxbuf);
		for (p_dat = iio_buffer_first(rxbuf, rx0_q); p_dat < p_end; p_dat += p_inc)
    {
			// Get I and Q and save to CSV
			const int16_t i = ((int16_t*)p_dat)[0]; // Real (I)
			const int16_t q = ((int16_t*)p_dat)[1]; // Imag (Q)
			fprintf(foutp, "%d,%d\n", i, q);
		}

		// WRITE: Get pointers to TX buf and write IQ to TX buf port 0
		p_inc = iio_buffer_step(txbuf);
		p_end = iio_buffer_end(txbuf);
		iter = 0;
		for (p_dat = iio_buffer_first(txbuf, tx0_q); p_dat < p_end; p_dat += p_inc)
    {
			((int16_t *)p_dat)[0] = QPSKsamplesTX[iter][0];
			((int16_t *)p_dat)[1] = QPSKsamplesTX[iter++][1];
		}
	}

	fclose(finp);
	fclose(foutp);

	shutdown();

	return 0;
}
