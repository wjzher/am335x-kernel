/*
 * agv fpga driver
 */
#include <linux/types.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/cdev.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/of_device.h>
#include <linux/gpio.h>
#include <linux/module.h>
#include <asm/irq.h>

#include "agv-fpga.h"

struct agv_fpga_priv fpga_priv;

static int agv_fpga_reset(struct agv_fpga_priv *pdata)
{
	gpio_set_value(pdata->rst_pin, 0);
	msleep(1);
	gpio_set_value(pdata->rst_pin, 1);
	return 0;
}

irqreturn_t agv_fpga_isr(int irq, void *p)
{
	struct agv_fpga_priv *pdata = (struct agv_fpga_priv *)p;
	complete(&pdata->int_event);
	return IRQ_HANDLED;
}

int agv_fpga_open(struct inode *inode, struct file *filp)
{
	struct agv_fpga_priv *pdata = &fpga_priv;

	agv_fpga_reset(pdata);

	pdata = container_of(inode->i_cdev, struct agv_fpga_priv, cdev);
	printk("priv = %p\n", pdata);
	// save data to private data
	filp->private_data = pdata;

	enable_irq(pdata->irq);
	return 0;
}

static long agv_fpga_ioctl(struct file* filp, unsigned int cmd, unsigned long arg)
{
	return 0;
}

static unsigned int agv_fpga_poll(struct file *filp, struct poll_table_struct *ptab)
{
	return 0;
}

static int agv_fpga_write(struct file *filp, const char __user *buf, size_t count, loff_t *offp)
{
	return 0;
}

static int agv_fpga_read(struct file *filp, char __user *buf, size_t count, loff_t *offp)
{
	struct agv_fpga_priv *pdata = (struct agv_fpga_priv *)filp->private_data;
	int ret;
	// timeout 1s
	ret = wait_for_completion_interruptible_timeout(&pdata->int_event, HZ);
	if (ret < 0) {
		return ret;
	}
	return ret;
}

static int agv_fpga_release(struct inode *inode, struct file *filp)
{
	struct agv_fpga_priv *pdata = (struct agv_fpga_priv *)filp->private_data;
	disable_irq(pdata->irq);
	return 0;
}

struct file_operations agv_fpga_fops = {
	.owner = THIS_MODULE,
	.open = agv_fpga_open,
	.unlocked_ioctl = agv_fpga_ioctl,
	.write = agv_fpga_write,
	.read = agv_fpga_read,
	.poll = agv_fpga_poll,
	.release = agv_fpga_release,
};

static int agv_fpga_register(struct agv_fpga_priv *pdata, struct device *dev)
{
	int ret;
	dev_t idev;

	ret = alloc_chrdev_region(&idev, 0, 1, dev_name(dev));
	if (ret) {
		dev_err(dev, "unable to alloc chrdev region\n");
		return ret;
	}

	pdata->idev = idev;
    cdev_init(&pdata->cdev, &agv_fpga_fops);  
    pdata->cdev.owner = THIS_MODULE;

	// add a character device
	ret = cdev_add(&pdata->cdev, idev, 1);
	if (ret) {
		dev_err(dev, "cdev_add failed\n");
		return ret;
	}

	// create device class
	pdata->_class = class_create(THIS_MODULE, dev_name(dev));
	if (IS_ERR(pdata->_class)) {
		dev_err(dev, "class_create failed\n");
		return ret;
	}
	// create the device node in /dev
	pdata->dev = device_create(pdata->_class, NULL,
		pdata->idev, pdata, "%s", dev_name(dev));
	if (IS_ERR(pdata->dev)) {
		dev_err(dev, "device_create failed\n");
		ret = PTR_ERR(pdata->dev);
		return ret;
	}
	return 0;
}

static int agv_fpga_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct agv_fpga_priv *pdata = &fpga_priv;
	struct device_node *pnode = pdev->dev.of_node;
	struct pinctrl *pinctrl;
	int ret;

	/* we only support OF */
	if (pnode == NULL) {
		dev_err(&pdev->dev, "No platform of_node!\n");
		return -ENODEV;
	}

	pinctrl = devm_pinctrl_get_select_default(&pdev->dev);
	if (IS_ERR(pinctrl)) {
		/* special handling for probe defer */
		if (PTR_ERR(pinctrl) == -EPROBE_DEFER)
			return -EPROBE_DEFER;

		dev_warn(&pdev->dev,
			"pins are not configured from the driver\n");
	}

	platform_set_drvdata(pdev, pdata);
	
	pdata->int_pin = of_get_named_gpio(pnode, "int-gpios", 0);
	pdata->rst_pin = of_get_named_gpio(pnode, "reset-gpios", 0);
	if (!gpio_is_valid(pdata->int_pin)) {
		dev_err(&pdev->dev, "No int pin!\n");
		ret = -ENODEV;
		goto err_no_gpio;
	}
	if (!gpio_is_valid(pdata->rst_pin)) {
		dev_err(&pdev->dev, "No reset pin!\n");
		ret = -ENODEV;
		goto err_no_gpio;
	}

	/* irq, gpio init */
	pdata->irq = gpio_to_irq(pdata->int_pin);
	ret = gpio_request(pdata->int_pin, "agv-fpga-int");
	if (ret) {
		dev_err(&pdev->dev, "gpio request int-pin failed %d\n", ret);
		goto err_no_gpio;
	}
	ret = gpio_direction_input(pdata->int_pin);
	if (ret) {
		dev_err(&pdev->dev, "gpio direction input int-pin failed %d\n", ret);
		goto err_no_gpio;
	}
	ret = gpio_request(pdata->rst_pin, "agv-fpga-reset");
	if (ret) {
		dev_err(&pdev->dev, "gpio request reset-pin failed %d\n", ret);
		goto err_no_gpio;
	}
	ret = gpio_direction_output(pdata->rst_pin, 1);
	if (ret) {
		dev_err(&pdev->dev, "gpio direction input reset-pin failed %d\n", ret);
		goto err_no_gpio;
	}

	/* request irq */
	ret = devm_request_irq(dev, pdata->irq, agv_fpga_isr,
		IRQF_TRIGGER_FALLING | IRQF_NO_THREAD, dev_name(dev), pdata);
	if (ret) {
		dev_err(&pdev->dev, "unable to request IRQ %d\n", pdata->irq);
		goto err_no_gpio;
	}
	disable_irq(pdata->irq);

	/* regiter device */
	agv_fpga_register(pdata, dev);

	init_completion(&pdata->int_event);
	
	dev_info(&pdev->dev, "ready, int-pin %d, reset-pin %d, irq %d\n",
		pdata->int_pin, pdata->rst_pin, pdata->irq);
	
	return 0;
err_class:
	class_unregister(pdata->_class);
err_irq:
	devm_free_irq(dev, pdata->irq, pdata);
err_no_gpio:
	devm_kfree(dev, pdata);
	return ret;
}

static int agv_fpga_remove(struct platform_device *pdev)
{
	struct agv_fpga_priv *pdata = (struct agv_fpga_priv *)platform_get_drvdata(pdev);
	
	gpio_free(pdata->int_pin);
	gpio_free(pdata->rst_pin);
	
	dev_info(&pdev->dev, "removing\n");
	return 0;
}

static const struct of_device_id agv_fpga_of_match[] = {
	{
		.compatible = "agv-fpga",
	},
	{ },
};
MODULE_DEVICE_TABLE(of, agv_fpga_of_match);

struct platform_driver agv_fpga_driver = {
	.probe		= agv_fpga_probe,
	.remove		= agv_fpga_remove,
	.driver = {
		.name		= "agv-fpga",
		.owner		= THIS_MODULE,
		.of_match_table	= agv_fpga_of_match,
	},
};

module_platform_driver(agv_fpga_driver);

MODULE_DESCRIPTION("AGV FPGA Driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("wjzhe");
MODULE_ALIAS("platform:agv-fpga");

