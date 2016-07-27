#ifndef AGV_FPGA_H
#define AGV_FPGA_H


struct agv_fpga_priv {
	struct cdev cdev;					/* Char device structure */
	struct device *dev;
	struct class *_class;
	struct completion int_event;
	dev_t idev;
	int int_pin;
	int rst_pin;
	int irq;
};


#endif

