/*
 * Copyright (C) 2009 Motorola, Inc.
 * Copyright (C) 2009 Google, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 * 02111-1307, USA
 */

#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/switch.h>
#include <linux/workqueue.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/bu52014hfv.h>

struct bu52014hfv_info {
	int gpio_north;
	int gpio_south;

	int irq_north;
	int irq_south;

	struct work_struct irq_north_work;
	struct work_struct irq_south_work;

	struct workqueue_struct *work_queue;
	struct switch_dev sdev;

	unsigned int north_value;
	unsigned int south_value;
};
enum {
	NO_DOCK,
	DESK_DOCK,
	CAR_DOCK,
};

static ssize_t print_name(struct switch_dev *sdev, char *buf)
{
	switch (switch_get_state(sdev)) {
	case NO_DOCK:
		return sprintf(buf, "None\n");
	case DESK_DOCK:
		return sprintf(buf, "DESK\n");
	case CAR_DOCK:
		return sprintf(buf, "CAR\n");
	}

	return -EINVAL;
}
static int bu52014hfv_update(struct bu52014hfv_info *info, int gpio, int dock)
{
	int state = !gpio_get_value(gpio);

	if (state)
		switch_set_state(&info->sdev, dock);
	else
		switch_set_state(&info->sdev, NO_DOCK);

	return state;
}

void bu52014hfv_irq_north_work_func(struct work_struct *work)
{
	struct bu52014hfv_info *info = container_of(work,
						    struct bu52014hfv_info,
						    irq_north_work);
	bu52014hfv_update(info, info->gpio_north, info->north_value);
	enable_irq(info->irq_north);
}

void bu52014hfv_irq_south_work_func(struct work_struct *work)
{
	struct bu52014hfv_info *info = container_of(work,
						    struct bu52014hfv_info,
						    irq_south_work);
	bu52014hfv_update(info, info->gpio_south, info->south_value);
	enable_irq(info->irq_south);
}

static irqreturn_t bu52014hfv_isr(int irq, void *dev)
{
	struct bu52014hfv_info *info = dev;

	disable_irq_nosync(irq);

	if (irq == info->irq_north)
		queue_work(info->work_queue, &info->irq_north_work);
	else if (irq == info->irq_south)
		queue_work(info->work_queue, &info->irq_south_work);

	return IRQ_HANDLED;
}
#ifdef CONFIG_OF
static struct bu52014hfv_platform_data *bu52014hfv_of_init(struct platform_device *pdev)
{
	struct bu52014hfv_platform_data *pdata;
	struct device_node *np = pdev->dev.of_node;
	unsigned int prop;

	pdata = devm_kzalloc(&pdev->dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata) {
		dev_err(&pdev->dev, "pdata allocation failure\n");
		return NULL;
	}
	if (!of_property_read_u32(np, "docked_north_gpio", &prop))
		pdata->docked_north_gpio = prop;
	if (!of_property_read_u32(np, "docked_south_gpio", &prop))
		pdata->docked_south_gpio = prop;
	if (!of_property_read_u32(np, "north_is_desk", &prop))
		pdata->north_is_desk = prop;

	return pdata;
}
#endif

static int bu52014hfv_probe(struct platform_device *pdev)
{
	struct bu52014hfv_platform_data *pdata;
	struct bu52014hfv_info *info;
	int ret = -1;

	info = kzalloc(sizeof(struct bu52014hfv_info), GFP_KERNEL);
	if (!info) {
		ret = -ENOMEM;
		pr_err("%s: could not allocate space for module data: %d\n",
		       __func__, ret);
		goto error_kmalloc_failed;
	}

	if (pdev->dev.of_node)
		pdata = bu52014hfv_of_init(pdev);
	else
		pdata = pdev->dev.platform_data;

	/* Initialize hall effect driver data */
	info->gpio_north = pdata->docked_north_gpio;
	info->gpio_south = pdata->docked_south_gpio;

	info->irq_north = gpio_to_irq(pdata->docked_north_gpio);
	info->irq_south = gpio_to_irq(pdata->docked_south_gpio);

	if (pdata->north_is_desk) {
		info->north_value = DESK_DOCK;
		info->south_value = CAR_DOCK;
	} else {
		info->north_value = CAR_DOCK;
		info->south_value = DESK_DOCK;
	}

	gpio_request(pdata->docked_north_gpio, "mapphone dock north");
	gpio_direction_input(pdata->docked_north_gpio);

	gpio_request(pdata->docked_south_gpio, "mapphone dock south");
	gpio_direction_input(pdata->docked_south_gpio);

	info->work_queue = create_singlethread_workqueue("bu52014hfv_wq");
	if (!info->work_queue) {
		ret = -ENOMEM;
		pr_err("%s: cannot create work queue: %d\n", __func__, ret);
		goto error_create_wq_failed;
	}
	INIT_WORK(&info->irq_north_work, bu52014hfv_irq_north_work_func);
	INIT_WORK(&info->irq_south_work, bu52014hfv_irq_south_work_func);

	ret = request_irq(info->irq_north, bu52014hfv_isr,
			  IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
			  BU52014HFV_MODULE_NAME, info);

	if (ret) {
		pr_err("%s: north request irq failed: %d\n", __func__, ret);
		goto error_request_irq_north_failed;
	}

	ret = request_irq(info->irq_south, bu52014hfv_isr,
			  IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
			  BU52014HFV_MODULE_NAME, info);
	if (ret) {
		pr_err("%s: south request irq failed: %d\n", __func__, ret);
		goto error_request_irq_south_failed;
	}

	enable_irq_wake(info->irq_north);
	enable_irq_wake(info->irq_south);

	info->sdev.name = "dock";
	info->sdev.print_name = print_name;
	ret = switch_dev_register(&info->sdev);
	if (ret) {
		pr_err("%s: error registering switch device %d\n",
			__func__, ret);
		goto error_switch_device_failed;
	}
	platform_set_drvdata(pdev, info);

	ret = bu52014hfv_update(info, info->gpio_south, info->south_value);
	if (!ret)
		bu52014hfv_update(info, info->gpio_north, info->north_value);

	printk("bu52014hfv probe ok!\n");
	return 0;

error_switch_device_failed:
	free_irq(info->irq_south, info);
error_request_irq_south_failed:
	free_irq(info->irq_north, info);
error_request_irq_north_failed:
	destroy_workqueue(info->work_queue);
error_create_wq_failed:
	kfree(info);
error_kmalloc_failed:
	return ret;
}

static int bu52014hfv_remove(struct platform_device *pdev)
{
	struct bu52014hfv_info *info = platform_get_drvdata(pdev);

	disable_irq_wake(info->irq_north);
	disable_irq_wake(info->irq_south);

	free_irq(info->irq_north, 0);
	free_irq(info->irq_south, 0);

	gpio_free(info->gpio_north);
	gpio_free(info->gpio_south);

	destroy_workqueue(info->work_queue);

	switch_dev_unregister(&info->sdev);

	kfree(info);
	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id bu52014hfv_id_table[] = {
	{ .compatible = "mot,bu52014hfv" },
	{}
};
MODULE_DEVICE_TABLE(of, bu52014hfv_id_table);
#endif

static const struct platform_device_id bu52014hfv_platform_id_table[] = {
	{"bu52014hfv", 0},
	{},
};
MODULE_DEVICE_TABLE(of, bu52014hfv_platform_id_table);


static struct platform_driver bu52014hfv_driver = {
	.probe = bu52014hfv_probe,
	.remove = bu52014hfv_remove,
	.driver = {
		   .name = "bu52014hfv",
		   .owner = THIS_MODULE,
#ifdef CONFIG_OF
		.of_match_table = of_match_ptr(bu52014hfv_id_table),
#endif
		   },
};

module_platform_driver(bu52014hfv_driver);

MODULE_DESCRIPTION("Rohm BU52014HFV Hall Effect Driver");
MODULE_AUTHOR("Motorola");
MODULE_LICENSE("GPL");
