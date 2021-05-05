include $(BARESIP_MOD_MK)

$(BUILD)/modules/$(MOD)/%.o: modules/$(MOD)/%.c $(BUILD) Makefile \
				modules/$(MOD)/module.mk mk/modules.mk
	@echo "  CC [M]  $@"
	@mkdir -p $(dir $@)
	$(HIDE)$(CC) $(CFLAGS) $($(call modulename,$@)_CFLAGS) \
		-c $< -o $@ $(DFLAGS)
